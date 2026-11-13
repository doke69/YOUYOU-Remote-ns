"""Execute actual NRO ELF receive/error instructions with a mocked BSD service.

Requires unicorn 2.1.4 and pyelftools 0.32. This is an ARM64 ABI regression test,
not a real Switch kernel, driver or network performance test.
"""
from pathlib import Path
import argparse, json, struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

p=argparse.ArgumentParser()
p.add_argument('--broken',type=Path,required=True)
p.add_argument('--fixed',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
args=p.parse_args()
results=[]
for label,path in [('broken',args.broken),('fixed',args.fixed)]:
    with path.open('rb') as stream:
        elf=ELFFile(stream)
        build_id=next(elf.get_section_by_name('.note.gnu.build-id').iter_notes())['n_desc']
        if label=='broken':broken_build_id=build_id
        else:assert build_id!=broken_build_id, 'Fixed ELF has not been rebuilt successfully'
        segments=[(s['p_vaddr'],s['p_memsz'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name and s['st_shndx']!='SHN_UNDEF'}
    # Symbols of STT_TLS contain offsets from the TLS template. Horizon's TP
    # points to its 16-byte TCB preceding that template, as in libnx codegen.
    for thread_pointer in (0x40100000,0x40110000):
        for scenario,bsd_return,bsd_errno,bsd_result,expected_errno in [
            ('empty',0,0,0,0),('EAGAIN',-1,11,0,11),('EINTR',-1,4,0,4),
            ('EIO',-1,5,0,5),('ENFILE',-1,-1,0xD201,23),
            ('EFAULT',-1,-1,0xD401,14),('EBUSY',-1,-1,0x10601,16),
            ('EPIPE',-1,-1,0x12345,32)]:
            for warm in (False,True):
                uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                end=max(address+size for address,size,data in segments)
                uc.mem_map(0,(end+4095)&~4095)
                for address,size,data in segments:uc.mem_write(address,data)
                uc.mem_map(0x40000000,0x400000)
                stop=0x40000000;scratch=0x40001000;messages=0x40002000
                vectors=0x40003000;names=0x40004000;buffer=0x40020000;payload=0x40040000
                handle=0x40000500;file_struct=0x40000600;device=0x40000700;device_name=0x40000800
                timeout=0x40000900;errno_address=thread_pointer+0x800
                uc.mem_write(handle,struct.pack('<IIQ',0,1,file_struct))
                uc.mem_write(file_struct,struct.pack('<i',73))
                uc.mem_write(device,struct.pack('<Q',device_name));uc.mem_write(device_name,b'soc\0')
                uc.mem_write(symbols['devoptab_list'],struct.pack('<Q',device))
                uc.mem_write(scratch,struct.pack('<QQ',buffer if warm else 0,69632 if warm else 0))
                for i in range(16):
                    uc.mem_write(vectors+i*16,struct.pack('<QQ',payload+i*4096,4096))
                    uc.mem_write(messages+i*56,struct.pack('<QI4xQi4xQIiI4x',names+i*128,128,vectors+i*16,1,0,0,0,0))
                uc.mem_write(thread_pointer+16+symbols['g_bsdErrno'],struct.pack('<i',bsd_errno))
                uc.mem_write(thread_pointer+16+symbols['g_bsdResult'],struct.pack('<I',bsd_result))
                faults=[];calls=[];tls_reads=[]
                hooks={symbols[name]:name for name in ('hosversionGet','__get_handle','strcmp','memset','__aarch64_read_tp','__errno','bsdRecvMMsg','__libnx_aligned_alloc')}

                def call_hook(machine,address,size,user):
                    name=hooks[address];calls.append(name)
                    x0=machine.reg_read(UC_ARM64_REG_X0);x1=machine.reg_read(UC_ARM64_REG_X1);x2=machine.reg_read(UC_ARM64_REG_X2)
                    if name=='hosversionGet':value=0x140000
                    elif name=='__get_handle':assert x0==3;value=handle
                    elif name=='strcmp':assert machine.mem_read(x0,4)==b'soc\0';value=0
                    elif name=='__aarch64_read_tp':value=thread_pointer;tls_reads.append(address)
                    elif name=='__errno':value=errno_address
                    elif name=='__libnx_aligned_alloc':assert x0==4096 and x1==69632;value=buffer
                    elif name=='memset':machine.mem_write(x0,bytes([x1&255])*x2);value=x0
                    else:
                        assert name=='bsdRecvMMsg' and x0==73 and x1==buffer and x2==69632
                        assert machine.reg_read(UC_ARM64_REG_X3)==16 and machine.reg_read(UC_ARM64_REG_X4)==0x80
                        assert machine.mem_read(x1,1)==b'\x08'
                        value=bsd_return & 0xffffffff
                    machine.reg_write(UC_ARM64_REG_X0,value)
                    machine.reg_write(UC_ARM64_REG_PC,machine.reg_read(UC_ARM64_REG_LR))

                def memory_fault(machine,access,address,size,value,user):
                    faults.append({'pc':hex(machine.reg_read(UC_ARM64_REG_PC)),'address':hex(address),'size':size})
                    return False

                for address in hooks:uc.hook_add(UC_HOOK_CODE,call_hook,begin=address,end=address)
                uc.hook_add(UC_HOOK_MEM_INVALID,memory_fault)
                for reg,value in [(UC_ARM64_REG_X0,3),(UC_ARM64_REG_X1,messages),(UC_ARM64_REG_X2,16),(UC_ARM64_REG_X3,0x80),(UC_ARM64_REG_X4,timeout),(UC_ARM64_REG_X5,scratch),(UC_ARM64_REG_LR,stop),(UC_ARM64_REG_SP,0x403fff00)]:uc.reg_write(reg,value)
                # Invalid memory is the expected result for the archived broken
                # binary. Catch only the emulator's execution exception.
                execution_error=None
                try:uc.emu_start(symbols['uu_recvmmsg_reuse'],stop,count=100000)
                except UcError as e:execution_error=str(e)
                expect_fault=label=='broken' and bsd_return==-1
                if expect_fault:
                    assert execution_error and len(faults)==1
                    assert faults[0]=={'pc':'0x112ee20','address':hex(thread_pointer*2),'size':4},faults
                else:
                    assert execution_error is None and not faults,(label,scenario,execution_error,faults)
                    assert uc.reg_read(UC_ARM64_REG_PC)==stop
                    assert uc.reg_read(UC_ARM64_REG_X0)&0xffffffff==bsd_return&0xffffffff
                    actual_errno=struct.unpack('<i',uc.mem_read(errno_address,4))[0]
                    assert actual_errno==expected_errno,(label,scenario,actual_errno)
                assert calls.count('__libnx_aligned_alloc')==(0 if warm else 1)
                assert struct.unpack('<QQ',uc.mem_read(scratch,16))==(buffer,69632)
                results.append({'binary':label,'scenario':scenario,'warm':warm,'tp':hex(thread_pointer),'faults':faults,'expected_fault':expect_fault,'passed':True})
args.output.write_text(json.dumps({'cases':len(results),'passed':True,'results':results,'limits':'Actual ARM64 application instructions; mocked SDK/IPC boundaries; not a device validation.'},indent=2))
print(f'PASS ARM64 cases={len(results)}; archived broken binary reproduces TP+TP faults; fixed binary returns expected BSD/IPC errno for two thread bases, cold and reused buffers')

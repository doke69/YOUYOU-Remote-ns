// Appended after the unmodified SDK baseline and the actual product adapter.
struct Slots {
    std::array<mmsghdr,32> messages{};
    std::array<iovec,32> vectors{};
    std::array<std::array<u8,4096>,32> data{};
    std::array<std::array<u8,128>,32> names{};
    std::array<std::array<u8,16>,32> controls{};
    void reset(unsigned n,unsigned controlBytes=16) {
        messages={}; vectors={}; data={}; names={}; controls={};
        for(unsigned i=0;i<n;++i){
            vectors[i]={data[i].data(),data[i].size()};
            messages[i].msg_hdr={names[i].data(),128,&vectors[i],1,
                controlBytes?controls[i].data():nullptr,controlBytes,0};
        }
    }
};
static unsigned scenario=0, controlBytes=16;
static bool baselineCall=true;
static std::vector<u8> expectedRequest;
int bsdRecvMMsg(int fd,void *buffer,size_t bytes,unsigned vlen,int flags,timespec *timeout) {
    assert(fd==73 && flags==0x80 && timeout->tv_sec==0 && timeout->tv_nsec==0);
    auto *b=static_cast<u8*>(buffer);
    assert(reinterpret_cast<uintptr_t>(b)%4096==0 && bytes%4096==0);
    if(baselineCall)expectedRequest.assign(b,b+bytes);
    else assert(expectedRequest.size()==bytes && std::memcmp(b,expectedRequest.data(),bytes)==0);
    if(scenario>=3 && scenario<=5){g_bsdErrno=9001+scenario-3;return -1;}
    if(scenario>=10 && scenario<=13){
        g_bsdErrno=-1;
        constexpr uint32_t results[]={0xD201,0xD401,0x10601,0x12345};
        g_bsdResult=results[scenario-10]; return -1;
    }
    if(scenario==6)return int(vlen)+1;
    Slots response; response.reset(vlen,controlBytes);
    const unsigned returned=scenario==1?1:scenario==2?0:vlen;
    constexpr size_t lengths[]={0,1,300,1200,4096};
    for(unsigned i=0;i<returned;++i){
        auto &msg=response.messages[i];
        msg.msg_hdr.msg_namelen=i%2?28:16;
        std::memset(response.names[i].data(),int(i+1),msg.msg_hdr.msg_namelen);
        response.vectors[i].iov_len=scenario==8?4096:lengths[i%5];
        std::memset(response.data[i].data(),int(11+i),response.vectors[i].iov_len);
        msg.msg_hdr.msg_controllen=scenario==9?controlBytes:0;
        msg.msg_hdr.msg_flags=scenario==8?0x10:0;
        msg.msg_len=scenario==8?5000:unsigned(response.vectors[i].iov_len);
    }
    assert(sdk::_serializeMmsg(b,bytes,response.messages.data(),returned,true)>=0);
    if(scenario==7){socklen_t bad=129;std::memcpy(b+1,&bad,sizeof bad);}
    return int(returned);
}
int main() {
    uu_batch_scratch scratch;
    size_t baselineAlloc=0,reusedAlloc=0,baselineFree=0,reusedFree=0,cases=0;
    for(unsigned repeat=0;repeat<10;++repeat)for(unsigned controls:{0u,16u})
    for(unsigned count:{1u,8u,16u,32u})for(scenario=0;scenario<14;++scenario){
        controlBytes=controls;
        Slots old,newer;old.reset(count,controlBytes);newer.reset(count,controlBytes);timespec timeout{};
        baselineCall=true;errno=0;auto a=allocations,f=frees;
        const int r1=sdk::recvmmsg(3,old.messages.data(),count,0x80,&timeout);const int e1=errno;
        baselineAlloc+=allocations-a;baselineFree+=frees-f;
        if(scratch.data)std::memset(scratch.data,0xcd,scratch.capacity);
        baselineCall=false;errno=0;a=allocations;f=frees;
        const int r2=uu_recvmmsg_reuse(3,newer.messages.data(),count,0x80,&timeout,&scratch);const int e2=errno;
        reusedAlloc+=allocations-a;reusedFree+=frees-f;
        assert(r1==r2 && e1==e2);
        assert(old.data==newer.data && old.names==newer.names && old.controls==newer.controls);
        for(unsigned i=0;i<count;++i){
            const auto &x=old.messages[i];const auto &y=newer.messages[i];
            assert(x.msg_len==y.msg_len && x.msg_hdr.msg_namelen==y.msg_hdr.msg_namelen &&
                x.msg_hdr.msg_controllen==y.msg_hdr.msg_controllen && x.msg_hdr.msg_flags==y.msg_hdr.msg_flags &&
                x.msg_hdr.msg_iovlen==y.msg_hdr.msg_iovlen && old.vectors[i].iov_len==newer.vectors[i].iov_len);
        }
        ++cases;
    }
    uu_recvmmsg_release(&scratch);++reusedFree;
    assert(scratch.data==nullptr && scratch.capacity==0);
    assert(baselineAlloc==1120 && baselineFree==1120 && reusedAlloc==4 && reusedFree==4);
    std::cout<<"PASS equivalence="<<cases<<" baseline_alloc_free="<<baselineAlloc<<'/'<<baselineFree
        <<" reused_alloc_free="<<reusedAlloc<<'/'<<reusedFree<<"; identical requests/results/payload/address/control/flags\n";

    Slots input;input.reset(16);timespec timeout{};
    // The fd mapping, pre-7.0 gate and argument checks must precede allocation.
    for(unsigned count:{0u,33u}){
        errno=0;const auto r1=sdk::recvmmsg(3,input.messages.data(),count,0x80,&timeout);const auto e1=errno;
        errno=0;const auto r2=uu_recvmmsg_reuse(3,input.messages.data(),count,0x80,&timeout,&scratch);
        assert(r1==r2 && e1==errno && r1==-1);++cases;
    }
    for(int fd:{-1,4}){
        errno=0;const auto r1=sdk::recvmmsg(fd,input.messages.data(),16,0x80,&timeout);const auto e1=errno;
        errno=0;const auto r2=uu_recvmmsg_reuse(fd,input.messages.data(),16,0x80,&timeout,&scratch);
        assert(r1==r2 && e1==errno && r1==-1);++cases;
    }
    assert(sdk::recvmmsg(3,nullptr,16,0x80,&timeout)==-1 && errno==EINVAL);
    assert(uu_recvmmsg_reuse(3,nullptr,16,0x80,&timeout,&scratch)==-1 && errno==EINVAL);++cases;
    oldFirmware=true;
    assert(sdk::recvmmsg(3,input.messages.data(),16,0x80,&timeout)==-1 && errno==ENOSYS);
    assert(uu_recvmmsg_reuse(3,input.messages.data(),16,0x80,&timeout,&scratch)==-1 && errno==ENOSYS);++cases;
    oldFirmware=false;
    assert(scratch.data==nullptr && scratch.capacity==0);
    failAllocation=true;
    assert(sdk::recvmmsg(3,input.messages.data(),16,0x80,&timeout)==-1 && errno==ENOMEM);
    failAllocation=true;
    assert(uu_recvmmsg_reuse(3,input.messages.data(),16,0x80,&timeout,&scratch)==-1 && errno==ENOMEM);++cases;
    assert(scratch.data==nullptr && scratch.capacity==0);

    // Independent registry lifetimes, growing, failed growth and reconnect.
    // scenario 2 returns no data but still validates all serialized request bytes.
    scenario=2;controlBytes=0;
    for(int run=0;run<5;++run){
        uu_batch_scratch other;
        for(unsigned count:{1u,16u}){
            input.reset(count,0);baselineCall=true;
            assert(sdk::recvmmsg(3,input.messages.data(),count,0x80,nullptr)==0);
            input.reset(count,0);baselineCall=false;
            assert(uu_recvmmsg_reuse(3,input.messages.data(),count,0x80,nullptr,&scratch)==0);
            input.reset(count,0);
            assert(uu_recvmmsg_reuse(3,input.messages.data(),count,0x80,nullptr,&other)==0);
            assert(scratch.data!=other.data && scratch.capacity==other.capacity);++cases;
        }
        auto *retained=scratch.data;auto capacity=scratch.capacity;
        assert(capacity==69632);input.reset(32,0);failAllocation=true;
        assert(uu_recvmmsg_reuse(3,input.messages.data(),32,0x80,nullptr,&scratch)==-1 && errno==ENOMEM);
        assert(scratch.data==retained && scratch.capacity==capacity);++cases;
        baselineCall=true;input.reset(32,0);assert(sdk::recvmmsg(3,input.messages.data(),32,0x80,nullptr)==0);
        baselineCall=false;input.reset(32,0);assert(uu_recvmmsg_reuse(3,input.messages.data(),32,0x80,nullptr,&scratch)==0);
        assert(scratch.capacity==139264);
        uu_recvmmsg_release(&scratch);uu_recvmmsg_release(&scratch);
        assert(other.data && other.capacity==69632);
        uu_recvmmsg_release(&other);++cases;
    }
    uint64_t a,f,live;uu_batch_rx_diagnostics(&a,&f,&live);
    assert(a==f && live==0 && allocations==frees);
    std::cout<<"PASS total="<<cases<<" adapter_allocations="<<a<<" frees="<<f<<" live_bytes="<<live
        <<"; FULL ZEROING RETAINED; mock BSD only; no device timing claim\n";
}

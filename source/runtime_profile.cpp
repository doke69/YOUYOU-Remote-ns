#include "runtime_profile.hpp"
#include <chrono>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace uu {
RuntimeProfile readRuntimeProfile() noexcept {
    RuntimeProfile out;
    const auto began=std::chrono::steady_clock::now();
#ifdef __SWITCH__
    out.isNso=envIsNso();out.heapOverride=envHasHeapOverride();
    out.appletType=int(appletGetAppletType());
    out.coreResult=svcGetInfo(&out.processCoreMask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0);
    s32 priority=-1;
    out.priorityResult=svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
    if(R_SUCCEEDED(out.priorityResult))out.mainPriority=priority;
    const auto init=clkrstInitialize();
    if(R_SUCCEEDED(init)){
        const PcvModuleId modules[]={PcvModuleId_CpuBus,PcvModuleId_GPU,PcvModuleId_EMC,PcvModuleId_NVDEC};
        for(unsigned i=0;i<out.clockHz.size();++i){
            ClkrstSession session{};
            auto rc=clkrstOpenSession(&session,modules[i],3);
            if(R_SUCCEEDED(rc)){
                rc=clkrstGetClockRate(&session,&out.clockHz[i]);
                clkrstCloseSession(&session);
            }
            out.clockResults[i]=rc;
            if(R_FAILED(rc))out.clockHz[i]=0;
        }
        clkrstExit();
    }else out.clockResults.fill(init);
#endif
    out.queryUs=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-began).count());
    out.sampled=true;return out;
}
}

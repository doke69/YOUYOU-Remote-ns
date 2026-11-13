#include "switch_thread_affinity.hpp"
#include "diagnostics.hpp"
#include <switch.h>
#include <atomic>
#include <cstdio>

extern "C" Result __real_svcStartThread(Handle handle);
extern "C" Result __real_svcSetThreadCoreMask(Handle handle, s32 preferred, u32 mask);
extern "C" Result __wrap_svcSetThreadCoreMask(Handle handle, s32 preferred, u32 mask);

namespace {
#ifdef UU_SWITCH_CORES_012
constexpr bool restrictCores=true;
#else
constexpr bool restrictCores=false;
#endif
// Startup/creation-only audit: no per-packet or per-frame sampling.
std::atomic<unsigned> starts{0}, changed{0}, applyFailures{0}, readFailures{0}, startFailures{0};
std::atomic<unsigned> core3Before{0}, core3After{0};
std::atomic<uint32_t> masksBefore{0}, masksAfter{0};
uint64_t mainMaskBefore=0, mainMaskAfter=0;

void auditThread(Handle handle, bool mainThread=false) noexcept {
    s32 preferred=-1;
    u64 before=0, after=0;
    if(R_FAILED(svcGetThreadCoreMask(&preferred,&before,handle))) {
        ++readFailures;
        return;
    }
    masksBefore.fetch_or(u32(before),std::memory_order_relaxed);
    if(before&8) ++core3Before;
    if(restrictCores && (before&~u64{7}))
        __wrap_svcSetThreadCoreMask(handle,preferred,u32(before));
    if(R_FAILED(svcGetThreadCoreMask(&preferred,&after,handle))) {
        ++readFailures;
        return;
    }
    masksAfter.fetch_or(u32(after),std::memory_order_relaxed);
    if(after&8) ++core3After;
    if(mainThread) {mainMaskBefore=before;mainMaskAfter=after;}
}
}

extern "C" Result __wrap_svcSetThreadCoreMask(Handle handle, s32 preferred, u32 mask) {
    if(!restrictCores || !(mask&~u32{7}))
        return __real_svcSetThreadCoreMask(handle,preferred,mask);
    u32 restricted=mask&7;
    if(!restricted) {
        u64 allowed=0;
        if(R_FAILED(svcGetInfo(&allowed,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)) || !(allowed&7)) {
            ++applyFailures;
            return __real_svcSetThreadCoreMask(handle,preferred,mask);
        }
        restricted=u32(allowed&7);
    }
    // A removed preferred core (including implicit process/default choices)
    // cannot remain the ideal core for the restricted affinity mask.
    const s32 ideal=(preferred>=0 && preferred<3 && (restricted&(1u<<preferred)))?preferred:-1;
    const auto result=__real_svcSetThreadCoreMask(handle,ideal,restricted);
    if(R_SUCCEEDED(result)) {++changed;return result;}
    ++applyFailures;
    // Keep the application's original behavior if the platform rejects this
    // experiment. The report explicitly prevents claiming affinity success.
    return __real_svcSetThreadCoreMask(handle,preferred,mask);
}

extern "C" Result __wrap_svcStartThread(Handle handle) {
    ++starts;
    auditThread(handle);
    const auto result=__real_svcStartThread(handle);
    if(R_FAILED(result)) ++startFailures;
    return result;
}

namespace uu {
void initializeThreadAffinity() noexcept {
    auditThread(CUR_THREAD_HANDLE,true);
    reportThreadAffinity("startup");
#ifdef UU_LEAN_STATS
    diagnostic("stats-mode-lean-cpu-sampling-off-decode-stages-off");
#endif
}
void reportThreadAffinity(const char *phase) noexcept {
    u64 allowed=0;
    const auto result=svcGetInfo(&allowed,InfoType_CoreMask,CUR_PROCESS_HANDLE,0);
    char text[512];
    std::snprintf(text,sizeof text,
        "thread-affinity-%s-mode-%s-process-mask-%llx-info-rc-%x-main-before-%llx-main-after-%llx-starts-%u-changed-%u-before-mask-%x-after-mask-%x-core3-before-%u-core3-after-%u-apply-failures-%u-read-failures-%u-start-failures-%u",
        phase,restrictCores?"cores012":"observe",(unsigned long long)allowed,unsigned(result),
        (unsigned long long)mainMaskBefore,(unsigned long long)mainMaskAfter,starts.load(),changed.load(),
        masksBefore.load(),masksAfter.load(),core3Before.load(),core3After.load(),
        applyFailures.load(),readFailures.load(),startFailures.load());
    diagnostic(text);
}
}

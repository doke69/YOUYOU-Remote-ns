#include "cpu_diagnostics.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace {
struct Slot {
    std::atomic<uint32_t> handle{0};
    std::atomic<uint64_t> threadId{0}, seen{0};
};
struct Role {
    std::atomic_flag writer=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> sequence{0}, seen{0}, marks{0}, changes{0}, skipped{0};
    // Roles 0 (SRTP workers) and 4 (dispatch workers) may run on up to four
    // shared ThreadPool workers; roles 1/2/3/5 are single-thread.
    std::array<Slot,4> slots{};
};
std::array<Role,6> roles;
constexpr bool multiSlot(unsigned role) { return role==0 || role==4; }
}
extern "C" void uu_cpu_mark(unsigned role) noexcept {
#ifdef __SWITCH__
    if(role>=roles.size()) return;
    thread_local std::array<uint64_t,6> last{};
    const auto now=armGetSystemTick();
    if(last[role] && now-last[role]<4800000) return;
    last[role]=now;
    thread_local uint64_t currentThreadId=0;
    thread_local bool haveThreadId=false;
    if(!haveThreadId) {
        if(R_FAILED(svcGetThreadId(&currentThreadId,CUR_THREAD_HANDLE))) {
            roles[role].skipped.fetch_add(1,std::memory_order_relaxed);return;
        }
        haveThreadId=true;
    }
    auto &r=roles[role];
    if(r.writer.test_and_set(std::memory_order_acquire)) {
        r.skipped.fetch_add(1,std::memory_order_relaxed);
        return; // No spinning or blocking in the packet callback.
    }
    ++r.sequence; // Odd while writing; SC fields give a coherent bounded read.
    const auto handle=threadGetCurHandle();
    const unsigned limit=multiSlot(role)?4u:1u;
    unsigned chosen=0;
    bool found=false;
    for(unsigned i=0;i<limit;++i) {
        if(r.slots[i].handle.load(std::memory_order_relaxed)==handle &&
           r.slots[i].threadId.load(std::memory_order_relaxed)==currentThreadId) {
            chosen=i;found=true;break;
        }
    }
    if(!found) {
        if(!multiSlot(role)) chosen=0;
        else {
            // Empty or stale (no mark for >3 s) first, otherwise the oldest.
            unsigned oldest=0;uint64_t oldestSeen=~uint64_t{0};
            for(unsigned i=0;i<4;++i) {
                const auto h=r.slots[i].handle.load(std::memory_order_relaxed);
                const auto seen=r.slots[i].seen.load(std::memory_order_relaxed);
                if(!h || (seen && now>seen && now-seen>57600000)) {chosen=i;found=true;break;}
                if(seen<oldestSeen) {oldestSeen=seen;oldest=i;}
            }
            if(!found) chosen=oldest;
        }
        if(r.slots[chosen].handle.exchange(handle,std::memory_order_relaxed)!=handle)
            ++r.changes;
        r.slots[chosen].threadId.store(currentThreadId,std::memory_order_relaxed);
        r.slots[chosen].seen.store(0,std::memory_order_relaxed);
    }
    r.slots[chosen].seen.store(now,std::memory_order_relaxed);
    r.seen.store(now,std::memory_order_relaxed);
    ++r.marks;
    ++r.sequence;
    r.writer.clear(std::memory_order_release);
#else
    (void)role;
#endif
}
namespace uu {
std::array<CpuRoleSnapshot,6> cpuDiagnosticsSnapshot() noexcept {
    std::array<CpuRoleSnapshot,6> out{};
#ifdef __SWITCH__
    // The UI/telemetry thread and the client thread may both request a
    // snapshot. Serialize the collector so a delta is never observed twice.
    static std::mutex snapshotMutex;
    std::lock_guard<std::mutex> snapshotLock(snapshotMutex);
    struct Previous {
        uint32_t handle=0;
        uint64_t threadId=0, ticks=0, at=0;
        std::array<uint64_t,4> core{{0,0,0,0}};
    };
    static std::array<std::array<Previous,4>,6> previous{};
    static uint64_t lastLedgerLog=0;
    const auto now=armGetSystemTick();
    std::array<std::array<std::pair<uint32_t,uint64_t>,4>,6> slotIds{};
    for(unsigned r=0;r<roles.size();++r) {
        uint64_t seq=0,marks=0,changes=0,skipped=0;
        std::array<std::pair<uint32_t,uint64_t>,4> ids{};
        std::array<uint64_t,4> seens{};
        bool coherent=false;
        for(unsigned attempt=0;attempt<2;++attempt) {
            seq=roles[r].sequence.load(std::memory_order_acquire);
            if(seq&1) continue;
            marks=roles[r].marks.load(std::memory_order_relaxed);
            changes=roles[r].changes.load(std::memory_order_relaxed);
            skipped=roles[r].skipped.load(std::memory_order_relaxed);
            for(unsigned i=0;i<4;++i) {
                ids[i]={roles[r].slots[i].handle.load(std::memory_order_relaxed),
                        roles[r].slots[i].threadId.load(std::memory_order_relaxed)};
                seens[i]=roles[r].slots[i].seen.load(std::memory_order_relaxed);
            }
            if(seq==roles[r].sequence.load(std::memory_order_acquire)) {coherent=true;break;}
        }
        out[r].marks=marks;out[r].ownershipChanges=changes;out[r].skippedMarks=skipped;
        if(!coherent) {previous[r]={};continue;}
        slotIds[r]=ids;
        bool any=false,mismatch=false;
        double sum=0;
        std::array<double,4> coreSum{{0,0,0,0}};
        uint64_t mask=0,newest=0;
        uint32_t error=0,valid=0;
        const unsigned limit=multiSlot(r)?4u:1u;
        for(unsigned i=0;i<limit;++i) {
            const auto handle=ids[i].first;
            if(!handle) continue;
            if(seens[i] && now>seens[i] && now-seens[i]>57600000) continue;
            s32 preferred=-1;uint64_t slotMask=0,threadId=0;
            auto rc=svcGetThreadCoreMask(&preferred,&slotMask,handle);
            if(R_SUCCEEDED(rc)) rc=svcGetThreadId(&threadId,handle);
            if(R_SUCCEEDED(rc) && threadId!=ids[i].second) {mismatch=true;previous[r][i]={};continue;}
            uint64_t total=0;
            std::array<uint64_t,4> coreTicks{{0,0,0,0}};
            if(R_SUCCEEDED(rc)) rc=svcGetInfo(&total,InfoType_ThreadTickCount,handle,TickCountInfo_Total);
            for(unsigned c=0;c<4 && R_SUCCEEDED(rc);++c)
                rc=svcGetInfo(&coreTicks[c],InfoType_ThreadTickCount,handle,uint64_t(c));
            if(R_FAILED(rc)) {error=rc;previous[r][i]={};continue;}
            auto &p=previous[r][i];
            if(p.handle==handle && p.threadId==threadId && p.at && now>p.at && total>=p.ticks) {
                const double elapsed=double(now-p.at);
                sum+=std::clamp(100.0*double(total-p.ticks)/elapsed,0.0,100.0);
                for(unsigned c=0;c<4;++c)
                    coreSum[c]+=std::clamp(100.0*double(coreTicks[c]-p.core[c])/elapsed,0.0,100.0);
                ++valid;any=true;
            }
            p={handle,threadId,total,now,coreTicks};
            mask|=slotMask;
            if(seens[i]>newest) newest=seens[i];
        }
        out[r].validSlots=valid;
        out[r].sampleValid=any;
        out[r].identityMismatch=mismatch;
        out[r].error=error;
        out[r].mask=mask;
        out[r].preferredCore=-1;
        out[r].aggregatePercent=any?sum:-1;
        // Schema-17 consumers expect a 0..100 percentage; keep the uncapped
        // multi-worker sum in aggregatePercent/corePercent for the local ledger.
        out[r].percent=any?std::min(100.0,sum):-1;
        for(unsigned c=0;c<4;++c) out[r].corePercent[c]=any?coreSum[c]:-1;
        out[r].ageMs=(any&&newest)?int64_t(armTicksToNs(now-newest)/1000000):-1;
    }
    for(unsigned i=0;i<roles.size();++i) {
        if(!out[i].sampleValid) continue;
        for(unsigned j=0;j<roles.size();++j) {
            if(i==j) continue;
            for(unsigned a=0;a<4;++a) {
                const auto ha=slotIds[i][a].first;
                if(!ha) continue;
                for(unsigned b=0;b<4;++b)
                    if(ha==slotIds[j][b].first && slotIds[i][a].second==slotIds[j][b].second)
                        out[i].sharedRoles|=1u<<j;
            }
        }
    }
    if(!lastLedgerLog || now-lastLedgerLog>=192000000) {
        char event[768];
        std::snprintf(event,sizeof(event),
            "cpu-ledger net=%.1f/%.1f c=%.1f,%.1f,%.1f,%.1f slots=%u dec=%.1f ui=%.1f ctl=%.1f "
            "drain=%.1f/%.1f c=%.1f,%.1f,%.1f,%.1f slots=%u poll=%.1f valid=%u%u%u%u%u%u shared=%u",
            out[0].aggregatePercent,out[0].percent,
            out[0].corePercent[0],out[0].corePercent[1],out[0].corePercent[2],out[0].corePercent[3],
            unsigned(out[0].validSlots),
            out[1].aggregatePercent,out[2].aggregatePercent,out[3].aggregatePercent,
            out[4].aggregatePercent,out[4].percent,
            out[4].corePercent[0],out[4].corePercent[1],out[4].corePercent[2],out[4].corePercent[3],
            unsigned(out[4].validSlots),out[5].aggregatePercent,
            unsigned(out[0].sampleValid),unsigned(out[1].sampleValid),unsigned(out[2].sampleValid),
            unsigned(out[3].sampleValid),unsigned(out[4].sampleValid),unsigned(out[5].sampleValid),
            unsigned(out[0].sharedRoles));
        diagnostic(event);
        lastLedgerLog=now;
    }
#endif
    return out;
}
}

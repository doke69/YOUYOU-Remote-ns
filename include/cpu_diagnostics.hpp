#pragma once
#include <array>
#include <cstdint>
extern "C" void uu_cpu_mark(unsigned role) noexcept;
namespace uu {
struct CpuRoleSnapshot {
    double percent=-1;
    // Per-core CPU sums across the valid slots of this role. -1 means no
    // valid sample. Values are percentages of one core and may exceed 100 for
    // a multi-thread role because they are summed, not normalized.
    std::array<double,4> corePercent{{-1,-1,-1,-1}};
    double aggregatePercent=-1;
    uint64_t mask=0, marks=0, ownershipChanges=0, skippedMarks=0;
    uint32_t error=0, sharedRoles=0, validSlots=0;
    int64_t ageMs=-1;
    int preferredCore=-1;
    bool sampleValid=false, identityMismatch=false;
};
// Roles:
// 0 = libdatachannel RTC workers executing SRTP recvMedia (up to 4 slots)
// 1 = media decode thread
// 2 = UI/main thread (marked at telemetry publish)
// 3 = client control thread
// 4 = authenticated media-dispatch consumer (up to 4 pool-worker slots)
// 5 = libjuice ICE/UDP poll thread
// The first four roles keep the schema-17 UDP field meanings (percent, mask,
// error); percent is capped to 100 for the multi-slot roles and the uncapped
// sum is available as aggregatePercent/corePercent to the local ledger.
// Callers are serialized; never infer actual cores from an affinity mask.
std::array<CpuRoleSnapshot,6> cpuDiagnosticsSnapshot() noexcept;
}

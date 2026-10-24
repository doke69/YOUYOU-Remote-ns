#pragma once
#include <array>
#include <cstdint>
namespace uu {
struct RuntimeProfile {
    bool sampled=false, isNso=false, heapOverride=false;
    std::array<uint32_t,4> clockHz{};
    std::array<uint32_t,4> clockResults{{~0u,~0u,~0u,~0u}};
    uint64_t processCoreMask=0, queryUs=0;
    uint32_t coreResult=~0u, priorityResult=~0u;
    int mainPriority=-1, appletType=-1;
};
// One explicit diagnostic request only. No setters, background polling, or files.
RuntimeProfile readRuntimeProfile() noexcept;
}

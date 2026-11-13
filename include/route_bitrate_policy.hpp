#pragma once
#include <algorithm>
#include <cstdint>

namespace uu {
enum class BitrateRoute { Unknown, Direct, Relay };
// Values have already been validated by the settings/config boundary. Zero
// means no user ceiling. Route uncertainty must not bypass relay protection.
inline int effectiveBitrateMbps(int userMbps, BitrateRoute route) noexcept {
    return route == BitrateRoute::Direct ? userMbps :
        (userMbps > 0 ? std::min(userMbps, 20) : 20);
}
inline uint32_t cappedFeedbackBps(uint32_t estimate, uint32_t ceiling) noexcept {
    return ceiling ? std::min(estimate, ceiling) : estimate;
}
}

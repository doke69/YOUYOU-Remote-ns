#pragma once

namespace uu {

// Named shortcuts for the same custom bitrate setting. These do not select
// the host's native quality enums or promise a sustained output bitrate.
struct QualityPreset {
    const char *name;
    int maxBitrateMbps;
};

inline constexpr QualityPreset qualityPresets[4] = {
    {"省流", 5},
    {"清晰", 12},
    {"高清", 20},
    {"原画", 30},
};
inline constexpr int qualityPresetCount = 4;

inline int qualityPresetIndexForMbps(int mbps) noexcept {
    for(int i=0;i<qualityPresetCount;++i)
        if(qualityPresets[i].maxBitrateMbps==mbps)return i;
    return -1;
}

struct QualityPresetRequest {
    int quality;
    int maxBitrateMbps;
};

inline QualityPresetRequest qualityPresetRequest(int index) noexcept {
    return {6,qualityPresets[index].maxBitrateMbps};
}

} // namespace uu

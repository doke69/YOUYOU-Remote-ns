#pragma once

namespace uu {

// The four production presets. legacyMbps is the pre-0.3.73 custom-cap label
// for that menu row. It is not a host output rate and must not be used to
// rewrite a saved custom cap into a preset.
struct QualityPreset {
    const char *name;
    int quality;
    int legacyMbps;
};

inline constexpr QualityPreset qualityPresets[4] = {
    {"省流", 1, 5},
    {"清晰", 2, 12},
    {"高清", 3, 20},
    {"原画", 4, 30},
};
inline constexpr int qualityPresetCount = 4;

inline int qualityPresetIndexForQuality(int quality) noexcept {
    for(int i=0;i<qualityPresetCount;++i)
        if(qualityPresets[i].quality==quality)return i;
    return -1;
}
inline int qualityPresetIndexForLegacyMbps(int mbps) noexcept {
    for(int i=0;i<qualityPresetCount;++i)
        if(qualityPresets[i].legacyMbps==mbps)return i;
    return -1;
}

struct QualityPresetRequest {
    int quality;
    int maxBitrateMbps;
};

// A loaded preset keeps its existing local cap when the user changes another
// setting; an explicit preset selection passes 0. Preset mode never writes a
// Custom field 8 at the protocol layer.
inline QualityPresetRequest qualityPresetRequest(int index,int requestedCap) noexcept {
    return {qualityPresets[index].quality,requestedCap};
}

} // namespace uu

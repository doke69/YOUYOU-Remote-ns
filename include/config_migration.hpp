#pragma once
#include "config_value.hpp"
#include "quality_presets.hpp"
#include <string>
#include <vector>

namespace uu {

struct QualityConfigMigration {
    bool changed=false;
    bool legacyCustom=false;
    bool invalidCustom=false;
    bool duplicateKeys=false;
    bool unresolved=false;
    int fromQuality=0;
    int fromMbps=0;
    int toQuality=0;
};

// v2 -> v3: exact old UI Custom caps become native presets. Inline comments
// are ignored for parsing and preserved on rewritten values. Reload and this
// migration both use the first occurrence of a key. Duplicate quality/cap
// entries remain unresolved and preserved (no key is rewritten), so the
// effective first value is not silently changed; this is the F02 compatibility
// contract for duplicate keys.
inline QualityConfigMigration migrateQualityConfigLines(std::vector<std::string> &lines) {
    QualityConfigMigration result;
    int qualityLine=-1,bitrateLine=-1,qualityCount=0,bitrateCount=0;
    for(int i=0;i<int(lines.size());++i) {
        const auto key=config::keyOf(lines[std::size_t(i)]);
        if(key=="quality") {
            if(qualityLine<0) qualityLine=i;
            ++qualityCount;
        } else if(key=="max_bitrate_mbps") {
            if(bitrateLine<0) bitrateLine=i;
            ++bitrateCount;
        }
    }
    if(qualityCount>1 || bitrateCount>1) {
        result.duplicateKeys=true;
        result.unresolved=true;
        return result;
    }
    if(qualityLine<0 || bitrateLine<0) {
        result.unresolved=true;
        return result;
    }
    int quality=0,mbps=0;
    const auto qualityValue=config::valueOf(lines[std::size_t(qualityLine)]);
    const auto bitrateValue=config::valueOf(lines[std::size_t(bitrateLine)]);
    if(!config::parseInt(qualityValue,quality) ||
       !config::parseInt(bitrateValue,mbps)) {
        result.unresolved=true;
        return result;
    }
    result.fromQuality=quality;
    result.fromMbps=mbps;
    if(quality>=1 && quality<=4)return result;
    if(quality!=6) {
        result.unresolved=true;
        return result;
    }
    const int preset=qualityPresetIndexForLegacyMbps(mbps);
    if(preset<0) {
        result.legacyCustom=true;
        result.invalidCustom=(mbps<=0);
        return result;
    }
    config::setValue(lines[std::size_t(qualityLine)],
        std::to_string(qualityPresets[preset].quality));
    config::setValue(lines[std::size_t(bitrateLine)],"0");
    result.changed=true;
    result.toQuality=qualityPresets[preset].quality;
    return result;
}

} // namespace uu

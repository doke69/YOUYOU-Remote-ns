#pragma once
#include "config_value.hpp"
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

// v2 -> v3 keeps a saved Custom ceiling. The old menu stored its four rows as
// quality=6 plus 5/12/20/30 Mbps. Rewriting those into presets 1..4 and
// clearing max_bitrate_mbps drops field 8; the host then applies its own
// preset ceilings (about 1/2/8/20 Mbps), so a saved 20 Mbps request becomes
// roughly 8 Mbps. Positive custom caps stay quality=6 with the original Mbps.
// Native presets (quality 1..4) are unchanged. Inline comments are ignored
// for parsing. Duplicate quality/cap entries stay unresolved and are not
// rewritten, so the effective first value is not silently changed.
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
    result.legacyCustom=true;
    result.invalidCustom=(mbps<=0);
    return result;
}

} // namespace uu

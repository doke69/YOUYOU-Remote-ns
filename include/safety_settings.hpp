#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace uu {
struct SafetySettings {
    bool autoLock=false;
    bool remoteMute=true;
    bool restoreSound=true;
};
inline SafetySettings safetySettingsFor(const nlohmann::json &profiles,const std::string &device) {
    SafetySettings result;
    if(!profiles.is_object())return result;
    const auto profile=profiles.find(device);
    if(profile==profiles.end() || !profile->is_object())return result;
    auto read=[&](const char *name,bool fallback) {
        const auto value=profile->find(name);
        return value!=profile->end() && value->is_boolean()?value->get<bool>():fallback;
    };
    result.autoLock=read("auto_lock",result.autoLock);
    result.remoteMute=read("remote_mute",result.remoteMute);
    result.restoreSound=read("restore_sound",result.restoreSound);
    return result;
}
inline unsigned remoteSoundAction(bool mute,bool restore) {
    return mute?(restore?18u:17u):16u;
}
inline std::vector<unsigned> remoteSoundActions(bool mute,bool restore) {
    // Mobile MuteSystem is a separate operation from the restoration policy.
    return mute?std::vector<unsigned>{14u,remoteSoundAction(true,restore)}:
                std::vector<unsigned>{16u};
}
} // namespace uu

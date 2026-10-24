#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace uu {
// Follow device identity across reordering; persist successful streams only.
class DeviceSelection {
    std::string selected_, recent_;
    static bool valid(const std::string &id) { return !id.empty() && id.size()<=256; }
public:
    void restore(const std::string &id) { if(valid(id))selected_=recent_=id; }
    bool choose(const nlohmann::json &devices,int index) {
        if(!devices.is_array() || index<0 || size_t(index)>=devices.size())return false;
        const auto &device=devices[size_t(index)];if(!device.is_object())return false;
        const auto id=device.find("device_id");
        if(id==device.end() || !id->is_string() || !valid(id->get_ref<const std::string&>()))return false;
        selected_=id->get<std::string>();return true;
    }
    int resolve(const nlohmann::json &devices)const {
        if(!devices.is_array())return 0;
        for(size_t i=0;i<devices.size();++i) {
            const auto &device=devices[i];if(!device.is_object())continue;
            const auto id=device.find("device_id");
            if(id!=device.end() && id->is_string() && id->get_ref<const std::string&>()==selected_)
                return int(i);
        }
        // Missing from a refreshed list does not erase the saved identity.
        return 0;
    }
    bool connected(const std::string &id) {
        if(!valid(id))return false;
        selected_=id;
        if(recent_==id)return false;
        recent_=id;return true;
    }
    const std::string &recent()const { return recent_; }
};
} // namespace uu

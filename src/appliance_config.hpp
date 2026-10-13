// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../third_party/json/json.hpp"
#include <array>
#include <string>
#include <algorithm>
#include <stdexcept>
namespace xone {
using Json=nlohmann::json;
struct ApplianceDevice {std::string name,profile,mac="000000000000";uint16_t pid,port;bool audio;};
inline std::array<ApplianceDevice,3> appliance_devices(const Json &config){
    std::array<ApplianceDevice,3> out{{{"new","headset-0b32-usb.json","000000000000",0x0b32,33240,true},{"old","headset-0b17-usb.json","000000000000",0x0b17,33242,true},{"controller","controller-0b12-usb.json","000000000000",0x0b12,33244,false}}};
    if(config.value("Version",1)!=1)throw std::runtime_error("Unsupported configuration version");
    std::array<bool,3> seen{};
    for(const auto &entry:config.value("Devices",Json::array())){
        const auto profile=entry.at("Profile").get<std::string>();
        auto slot=std::find_if(out.begin(),out.end(),[&](const auto &d){return d.profile==profile;});
        if(slot==out.end() || seen[static_cast<size_t>(slot-out.begin())])throw std::runtime_error("Unsupported or repeated device model");
        seen[static_cast<size_t>(slot-out.begin())]=true;
        auto mac=entry.value("Mac",std::string("000000000000"));
        std::transform(mac.begin(),mac.end(),mac.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(mac.size()!=12 || !std::all_of(mac.begin(),mac.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw std::runtime_error("Invalid saved device address");
        slot->mac=std::move(mac);
    }
    for(size_t a=0;a<out.size();++a)for(size_t b=a+1;b<out.size();++b)
        if(out[a].mac!="000000000000" && out[a].mac==out[b].mac)throw std::runtime_error("Repeated saved device address");
    return out;
}
inline uint16_t usb_model(uint16_t radio_product){
    if(radio_product==0x0b31)return 0x0b17; // Observed 2021 RF identity / wired USB profile.
    if(radio_product==0x0b13)return 0x0b12; // Series wireless / wired transport identities.
    return radio_product;
}
}

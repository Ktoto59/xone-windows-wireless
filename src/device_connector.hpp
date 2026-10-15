// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <chrono>
#include <stdexcept>
namespace xone {
struct DeviceConnector {
    uint16_t pid;const char *name;bool audio,independent_clock,preserve_wireless;
    std::chrono::milliseconds render_age;
};
inline DeviceConnector device_connector(uint16_t pid){
    switch(pid){
    case 0x0b17:case 0x0b31:return {0x0b17,"headset-2021",true,true,true,std::chrono::milliseconds(96)};
    case 0x0b32:return {0x0b32,"headset-2024",true,false,false,std::chrono::milliseconds(64)};
    case 0x0b12:case 0x0b13:return {0x0b12,"series-controller",false,false,false,std::chrono::milliseconds(64)};
    default:throw std::runtime_error("Unsupported Xbox radio model");
    }
}
}

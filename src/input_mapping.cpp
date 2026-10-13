// SPDX-License-Identifier: GPL-2.0-or-later
// GIP layout from xone/driver/gamepad.c, Copyright (C) 2021 Severin von Wnuck-Lipinski.
#include "input_output.hpp"
#include <algorithm>
namespace xone {
VirtualReport virtual_report(const GamepadInput &p,bool guide) {
    uint16_t buttons=guide?0x400:0;
    constexpr uint16_t destinations[]={0x10,0x20,0x1000,0x2000,0x4000,0x8000,1,2,4,8,0x100,0x200,0x40,0x80};
    for(unsigned i=0;i<14;++i) if(p.buttons&(1u<<(i+2))) buttons|=destinations[i];
    auto trigger=[](uint16_t value){return static_cast<uint8_t>((std::min(unsigned(value),1023u)*255+511)/1023);};
    return {buttons,trigger(p.left_trigger),trigger(p.right_trigger),p.left_x,p.left_y,p.right_x,p.right_y};
}
std::array<uint8_t,9> rumble_packet(uint8_t large,uint8_t small_motor) {
    auto motor=[](uint8_t value){return static_cast<uint8_t>((unsigned(value)*100+127)/255);};
    // Same motor mask/duration/repeat as upstream; trigger motors are disabled in XInput mode.
    return {0,15,0,0,motor(large),motor(small_motor),255,0,235};
}
std::array<uint8_t,14> native_input_report(const GamepadInput &p) {
    auto trigger=[](uint16_t v){return static_cast<uint16_t>((std::min(unsigned(v),1023u)*65535+511)/1023);};
    const uint16_t fields[]={static_cast<uint16_t>(p.buttons&0xfffc),trigger(p.left_trigger),trigger(p.right_trigger),static_cast<uint16_t>(p.left_x),static_cast<uint16_t>(p.left_y),static_cast<uint16_t>(p.right_x),static_cast<uint16_t>(p.right_y)};
    std::array<uint8_t,14> out{};for(unsigned i=0;i<7;++i){out[2*i]=static_cast<uint8_t>(fields[i]);out[2*i+1]=static_cast<uint8_t>(fields[i]>>8);}return out;
}
std::optional<std::array<uint8_t,9>> native_rumble_report(std::span<const uint8_t> r) {
    if(r.size()!=10 || r[0]>15 || r[1]>100 || r[2]>100 || r[3]>100 || r[4]>100)return {};
    auto duration=[](unsigned ms){return static_cast<uint8_t>(std::min((ms+9)/10,255u));};
    return std::array<uint8_t,9>{0,r[0],r[1],r[2],r[3],r[4],duration(r[5]|(unsigned(r[6])<<8)),duration(r[7]|(unsigned(r[8])<<8)),r[9]};
}
}

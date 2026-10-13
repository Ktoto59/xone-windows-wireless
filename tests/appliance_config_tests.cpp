// SPDX-License-Identifier: GPL-2.0-or-later
#include "appliance_config.hpp"
#include <iostream>
int main(){try{
    using namespace xone;const auto empty=appliance_devices(Json::object());if(empty[0].pid!=0x0b32 || empty[1].pid!=0x0b17 || empty[2].pid!=0x0b12)throw std::runtime_error("Wrong automatic models");
    auto config=Json{{"Version",1},{"Devices",Json::array({{{"Profile","headset-0b32-usb.json"},{"Mac","020000000001"}}})}};if(appliance_devices(config)[0].mac!="020000000001")throw std::runtime_error("Saved association lost");
    unsigned rejected=0;for(auto bad:{Json{{"Version",2}},Json{{"Devices",Json::array({{{"Profile","unknown"}}})}},Json{{"Devices",Json::array({{{"Profile","headset-0b32-usb.json"},{"Mac","invalid"}}})}}}){try{appliance_devices(bad);}catch(const std::exception &){++rejected;}}
    if(rejected!=3 || usb_model(0x0b31)!=0x0b17 || usb_model(0x0b13)!=0x0b12)throw std::runtime_error("Invalid configuration/model accepted");
    std::cout<<"PASS automatic model catalog, saved pairing, input validation and RF/USB model aliases\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

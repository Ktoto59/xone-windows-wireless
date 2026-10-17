// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_import_recovery.hpp"
#include <iostream>
#include <stdexcept>
using xone::UsbImportRecovery;using namespace std::chrono_literals;
void require(bool value){if(!value)throw std::runtime_error("USB import recovery assertion failed");}
int main(){try{
    auto t=UsbImportRecovery::Clock::now();
    require(!UsbImportRecovery::detach(true,false,1,1,false,true,t,t+1s));
    require(UsbImportRecovery::detach(true,false,1,1,false,true,t,t+3s));
    require(!UsbImportRecovery::detach(true,false,1,1,true,true,t,t+3s));
    require(!UsbImportRecovery::detach(true,false,1,1,false,false,t,t+3s));
    require(UsbImportRecovery::detach(true,false,2,1,true,true,t,t));
    require(UsbImportRecovery::detach(false,false,1,1,true,true,t,t));
    require(UsbImportRecovery::detach(false,true,2,1,true,true,t,t));
    require(!UsbImportRecovery::detach(false,true,2,2,false,true,t,t+1s));
    require(UsbImportRecovery::detach(false,true,2,2,false,true,t,t+3s));
    require(UsbImportRecovery::attach(false,true,true));
    require(!UsbImportRecovery::attach(false,true,false));
    require(!UsbImportRecovery::attach(false,false,true));
    std::cout<<"PASS import recovery: closed TCP, generation, stale snapshots, grace and RF-preserving retry\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_import_recovery.hpp"
#include "usbip_client_progress.hpp"
#include <iostream>
#include <stdexcept>
using xone::UsbImportRecovery;using namespace std::chrono_literals;
void require(bool value){if(!value)throw std::runtime_error("USB import recovery assertion failed");}
int main(){try{
    const auto t=UsbImportRecovery::Clock::time_point{}+1h;
    UsbImportRecovery r;r.begin(10,1,0,t);
    auto observe=[&](bool imported,uint64_t seq,auto now,uint64_t imports=1,uint64_t closed=0){return r.detach(true,false,1,1,imported,true,seq,imports,closed,now);};
    require(!observe(false,10,t+1h)); // stale before-attach snapshot
    require(!observe(false,11,t+2s));require(!observe(false,12,t+4s));
    require(!observe(false,12,t+1h)); // polling cannot confirm freshness
    require(!observe(true,13,t+12s,2)); // slow publication, no false reimport
    require(!observe(true,14,t+2h,2)); // indefinite silence/idle
    require(observe(false,15,t+2h,2,1)); // confirmed TCP close
    r.begin(20,2,1,t);require(observe(false,21,t+1ms,3,2)); // coalesced open/close
    r.begin(20,2,1,t);require(!observe(false,21,t+3s,2,1));
    require(!r.detach(true,false,1,1,false,false,22,2,1,t+5s));
    require(!observe(false,23,t+9s,2,1));require(!observe(false,24,t+9999ms,2,1));
    require(observe(false,25,t+10s,2,1)); // distinct live confirmations
    r.begin(30,2,1,t);
    require(r.detach(true,false,2,1,true,true,30,2,1,t));
    require(r.detach(false,false,1,1,true,true,30,2,1,t));
    require(!r.detach(false,true,1,1,false,true,30,2,1,t+1h)); // pre-attach recovery
    require(UsbImportRecovery::attach(false,true,true));
    require(!UsbImportRecovery::attach(false,true,false));
    require(!UsbImportRecovery::attach(false,false,true));
    // Composite Generation collision after repeated recoveries: 10*100+101
    // equals 11*100+1. A new route also restarts snapshot/counter values.
    r.begin(300,100,99,t,10);
    require(r.detach(true,false,1101,1101,false,true,1,0,0,t+1s,11));
    r.begin(10,1,0,t,7,1000);
    require(!r.detach(true,false,1,1,false,true,11,1,1,t+1s,7,999)); // old close publication
    require(!r.detach(true,false,1,1,true,true,12,2,1,t+1s,7,1001)); // new import is healthy
    require(r.detach(true,false,1,1,false,true,13,2,2,t+2s,7,1002));
    r.begin(10,0,0,t,7,1000);
    require(!r.detach(true,false,1,1,false,true,11,1,1,t+1s,7,1000)); // equal uptime granularity
    require(r.detach(true,false,1,1,false,true,12,1,1,t+2s,7,1001)); // confirmed fast failure
    xone::UsbipClientProgress tcp;
    require(!tcp.stalled(t+24h));tcp.queued(t);
    for(auto pause:{10ms,20ms,40ms,80ms,250ms,500ms})require(!tcp.stalled(t+pause));
    tcp.queued(t+14s);require(tcp.stalled(t+15s)); // queued data is not progress
    tcp.sent(t+15s,true);require(!tcp.stalled(t+29s));
    tcp.sent(t+29s,false);require(!tcp.stalled(t+24h));
    std::cout<<"PASS import recovery: fresh publications, slow import, TCP close, missing snapshots, silence and write progress\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

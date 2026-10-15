// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic radio only: exercises the production automatic route factory.
#include "native_device_route.hpp"
#include "auto_gip_router.hpp"
#include <iostream>
#include <thread>
#include <mutex>
int main(int argc,char **argv){
    if(argc!=2)return 2;
    try{
        const auto base=static_cast<uint16_t>(std::stoul(argv[1]));
        xone::AutoGipRouter router(4,[&](size_t n,const xone::Bytes &mac){return std::make_unique<xone::NativeDeviceRoute>(static_cast<uint16_t>(base+2*n),mac,L"",false);});
        auto add=[&](uint8_t wcid,uint16_t pid){
            const xone::Bytes mac{2,0,0,0,0,wcid};router.event(1,wcid,mac);
            xone::Bytes announce(28);announce[8]=0x5e;announce[9]=4;announce[10]=static_cast<uint8_t>(pid);announce[11]=static_cast<uint8_t>(pid>>8);
            router.event(2,wcid,xone::encode_gip({2,32,1,0,announce}));
        };
        add(1,0x0b31);add(2,0x0b32);add(3,0x0b13);add(4,0x0b32);
        std::mutex mutex;
        std::jthread ack([&](std::stop_token stop){while(!stop.stop_requested()){
            {std::lock_guard lock(mutex);for(const auto &tx:router.poll())router.complete(tx.wcid,tx.token,0);}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }});
        std::cout<<"READY\n"<<std::flush;std::string command;
        while(std::getline(std::cin,command)){
            std::lock_guard lock(mutex);
            if(command=="remove-old")router.event(3,1);
            else if(command=="add-old")add(1,0x0b31);
            else if(command=="quit")break;
            std::cout<<"OK\n"<<std::flush;
        }
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

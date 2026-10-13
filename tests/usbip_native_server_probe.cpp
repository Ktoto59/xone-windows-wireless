// SPDX-License-Identifier: GPL-2.0-or-later
// Test harness: only loopback TCP and synthetic RF events; never opens WinUSB.
#include "usbip_native_output.hpp"
#include "gip_link.hpp"
#include <iostream>
#include <thread>
int main(int argc,char **argv){
    if(argc!=3)return 2;
    try{
        const auto port=static_cast<uint16_t>(std::stoul(argv[1])),pid=static_cast<uint16_t>(std::stoul(argv[2],nullptr,16));
        const xone::Bytes mac{1,2,3,4,5,6};xone::NativeUsbOutput server(port,mac,pid,pid!=0x0b12,L"");server.event(1,1,mac);
        std::jthread ack([&](std::stop_token stop){while(!stop.stop_requested()){
            try{for(const auto &tx:server.poll())server.complete(tx.wcid,tx.token,0);}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }});
        std::cout<<"READY\n"<<std::flush;std::string command;
        while(std::getline(std::cin,command)){
            if(command=="audio"){xone::Bytes pcm(386);pcm[1]=6;for(size_t i=2;i<pcm.size();++i)pcm[i]=static_cast<uint8_t>(i);const auto wire=xone::encode_gip({0x60,32,1,0,pcm});server.event(4,1,wire);}
            else if(command=="input")server.event(2,1,xone::Bytes{3,32,1,4,0,0,0,0});
            else if(command=="quit")break;
        }return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

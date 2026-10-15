// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "raw_gip.hpp"
#include "radio_ipc_queue.hpp"
#include <deque>
#include <algorithm>
#include <stdexcept>
namespace xone {
struct UsbGipBroker::Impl {
    SOCKET socket=INVALID_SOCKET;bool started=false;Bytes mac,input;
    RadioIpcQueue output;
    ~Impl(){if(socket!=INVALID_SOCKET)closesocket(socket);if(started)WSACleanup();}
    void queue(uint8_t kind,uint8_t wcid,uint32_t token,std::span<const uint8_t> data){
        if(data.size()>1600)throw std::runtime_error("USB GIP broker frame limit");
        Bytes b{'X','G','P','1',kind,wcid,static_cast<uint8_t>(data.size()),static_cast<uint8_t>(data.size()>>8)};
        for(unsigned n=0;n<4;++n)b.push_back(static_cast<uint8_t>(token>>(8*n)));
        b.insert(b.end(),data.begin(),data.end());output.push(std::move(b),kind==4);
    }
    void flush(){
        for(unsigned n=0;n<128;++n){
            const auto b=output.next();if(b.empty())break;
            const auto result=send(socket,reinterpret_cast<const char *>(b.data()),static_cast<int>(b.size()),0);
            if(result==SOCKET_ERROR){if(WSAGetLastError()==WSAEWOULDBLOCK)return;throw std::runtime_error("USB GIP broker send failed");}
            if(!result)throw std::runtime_error("USB GIP broker closed");output.advance(static_cast<size_t>(result));
        }
    }
};
UsbGipBroker::UsbGipBroker(uint16_t port,Bytes mac):impl_(std::make_unique<Impl>()){
    if(port<1024 || mac.size()!=6)throw std::runtime_error("Invalid local USB GIP broker configuration");
    impl_->mac=std::move(mac);WSADATA startup{};
    if(WSAStartup(MAKEWORD(2,2),&startup))throw std::runtime_error("Winsock startup failed");impl_->started=true;
    impl_->socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(impl_->socket==INVALID_SOCKET)throw std::runtime_error("USB GIP socket failed");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(impl_->socket,reinterpret_cast<const sockaddr *>(&address),sizeof(address)))throw std::runtime_error("Start the local USB GIP server before the radio bridge");
    u_long nonblocking=1;if(ioctlsocket(impl_->socket,FIONBIO,&nonblocking))throw std::runtime_error("USB GIP nonblocking socket failed");
    const BOOL no_delay=TRUE;setsockopt(impl_->socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char *>(&no_delay),sizeof(no_delay));
    impl_->queue(0,0,0,impl_->mac);impl_->flush();
}
UsbGipBroker::~UsbGipBroker()=default;
bool UsbGipBroker::target(std::span<const uint8_t> mac) const{return std::equal(mac.begin(),mac.end(),impl_->mac.begin(),impl_->mac.end());}
void UsbGipBroker::event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data){impl_->queue(kind,wcid,0,data);}
void UsbGipBroker::complete(uint8_t wcid,uint32_t token,int status){
    const auto value=static_cast<uint32_t>(status);Bytes b;for(unsigned n=0;n<4;++n)b.push_back(static_cast<uint8_t>(value>>(8*n)));
    impl_->queue(17,wcid,token,b);
}
std::vector<RawGipTransmit> UsbGipBroker::poll(){
    impl_->flush();std::array<char,8192> buffer{};
    for(unsigned n=0;n<16;++n){
        const auto length=recv(impl_->socket,buffer.data(),static_cast<int>(buffer.size()),0);
        if(length==SOCKET_ERROR){if(WSAGetLastError()==WSAEWOULDBLOCK)break;throw std::runtime_error("USB GIP broker receive failed");}
        if(!length)throw std::runtime_error("USB GIP broker disconnected");
        if(impl_->input.size()+static_cast<size_t>(length)>256*1024)throw std::runtime_error("USB GIP receive buffer limit");
        impl_->input.insert(impl_->input.end(),buffer.data(),buffer.data()+length);
    }
    std::vector<RawGipTransmit> result;size_t offset=0;
    while(impl_->input.size()-offset>=12 && result.size()<64){
        const auto *p=impl_->input.data()+offset;
        if(!std::equal(p,p+4,"XGP1") || (p[4]!=16 && p[4]!=18) || !p[5] || p[5]>16)throw std::runtime_error("Invalid USB GIP broker frame");
        const size_t size=p[6]|(size_t(p[7])<<8);if(size>(p[4]==18?1600u:64u))throw std::runtime_error("USB GIP request exceeds endpoint limit");
        if(impl_->input.size()-offset<12+size)break;
        uint32_t token=0;for(unsigned n=0;n<4;++n)token|=uint32_t(p[8+n])<<(8*n);
        if(!token)throw std::runtime_error("Missing USB GIP completion token");
        result.push_back({p[5],token,Bytes(p+12,p+12+size),p[4]==18});offset+=12+size;
    }
    impl_->input.erase(impl_->input.begin(),impl_->input.begin()+offset);return result;
}
}

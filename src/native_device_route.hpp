// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "usbip_native_output.hpp"
#include "device_connector.hpp"
#include "gip_link.hpp"
namespace xone {
// Created per association, model selected before any Windows USB import.
class NativeDeviceRoute final:public RawGipOutput {
    uint16_t port_;Bytes mac_;std::wstring status_;bool diagnostics_;uint8_t wcid_=0;
    std::unique_ptr<NativeUsbOutput> peer_;
public:
    NativeDeviceRoute(uint16_t port,Bytes mac,std::wstring status,bool diagnostics):port_(port),mac_(std::move(mac)),status_(std::move(status)),diagnostics_(diagnostics){}
    bool target(std::span<const uint8_t> mac)const override{return std::equal(mac.begin(),mac.end(),mac_.begin(),mac_.end());}
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})override{
        if(kind==1){wcid_=wcid;return;}
        if(!peer_ && kind==2)for(const auto &p:parse_gip(data))if(p.command==2 && (p.options&63)==32 && p.payload.size()>=28){
            const auto vendor=p.payload[8]|(unsigned(p.payload[9])<<8),product=p.payload[10]|(unsigned(p.payload[11])<<8);
            if(vendor!=0x045e)throw std::runtime_error("Unsupported radio vendor");const auto config=device_connector(static_cast<uint16_t>(product));
            peer_=std::make_unique<NativeUsbOutput>(port_,mac_,config.pid,config.audio,status_,true,diagnostics_);peer_->event(1,wcid_,mac_);break;
        }
        if(peer_)peer_->event(kind,wcid,data);
    }
    std::vector<RawGipTransmit> poll()override{return peer_?peer_->poll():std::vector<RawGipTransmit>{};}
    void complete(uint8_t wcid,uint32_t token,int status)override{if(peer_)peer_->complete(wcid,token,status);}
};
}

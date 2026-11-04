// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "usbip_native_output.hpp"
#include "device_connector.hpp"
#include "gip_link.hpp"
#include "announce_watchdog.hpp"
namespace xone {
// Created per association, model selected before any Windows USB import.
class NativeDeviceRoute final:public RawGipOutput {
public:
    using Factory=std::function<std::unique_ptr<RawGipOutput>(uint16_t,bool)>;
    using Now=std::function<Time()>;
private:
    uint16_t port_;Bytes mac_;std::wstring status_;bool diagnostics_;uint8_t wcid_=0;
    std::unique_ptr<RawGipOutput> peer_;
    NativeUsbOutput::StateSink sink_;
    AnnounceWatchdog announce_;Factory factory_;Now now_;
public:
    NativeDeviceRoute(uint16_t port,Bytes mac,std::wstring status,bool diagnostics,NativeUsbOutput::StateSink sink={},Now now=std::chrono::steady_clock::now,Factory factory={})
        :port_(port),mac_(std::move(mac)),status_(std::move(status)),diagnostics_(diagnostics),sink_(std::move(sink)),factory_(std::move(factory)),now_(std::move(now)){
        if(!now_)throw std::runtime_error("Missing route clock");
        if(!factory_)factory_=[this](uint16_t pid,bool audio){return std::make_unique<NativeUsbOutput>(port_,mac_,pid,audio,status_,true,diagnostics_,sink_);};
    }
    bool target(std::span<const uint8_t> mac)const override{return std::equal(mac.begin(),mac.end(),mac_.begin(),mac_.end());}
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})override{
        event_at(kind,wcid,data,now_());
    }
    void event_at(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data,Time arrived)override{
        if(kind==1){wcid_=wcid;announce_.arm(std::min(arrived,now_()));return;}
        if(kind==3){peer_.reset();wcid_=0;announce_.clear();return;}
        if(!peer_ && kind==2)for(const auto &p:parse_gip(data))if(p.command==2 && (p.options&63)==32 && p.payload.size()>=28){
            const auto vendor=p.payload[8]|(unsigned(p.payload[9])<<8),product=p.payload[10]|(unsigned(p.payload[11])<<8);
            if(vendor!=0x045e)throw std::runtime_error("Unsupported radio vendor");const auto config=device_connector(static_cast<uint16_t>(product));
            peer_=factory_(config.pid,config.audio);
            if(!peer_)throw std::runtime_error("Missing native USB route");
            peer_->event_at(1,wcid_,mac_,arrived);announce_.clear();break;
        }
        if(peer_)peer_->event_at(kind,wcid,data,arrived);
    }
    std::vector<RawGipTransmit> poll()override{
        // AutoGipRouter turns this into a per-peer fault. Session's existing
        // backoff owns RF re-enumeration; never restart the entire USB adapter.
        if(announce_.expired(now_()))throw std::runtime_error("Initial GIP announcement timed out");
        return peer_?peer_->poll():std::vector<RawGipTransmit>{};
    }
    void complete(uint8_t wcid,uint32_t token,int status)override{if(peer_)peer_->complete(wcid,token,status);}
    void complete_at(uint8_t wcid,uint32_t token,int status,Time finished)override{if(peer_)peer_->complete_at(wcid,token,status,finished);}
};
}

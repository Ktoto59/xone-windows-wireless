// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "protocol.hpp"
#include <memory>
namespace xone {
using Bytes=std::vector<uint8_t>;
struct RawGipTransmit { uint8_t wcid; uint32_t token; Bytes data; bool audio=false; };
class RawGipOutput {
public:
    virtual ~RawGipOutput()=default;
    virtual bool target(std::span<const uint8_t> mac) const=0;
    virtual bool automatic()const{return false;}
    virtual bool audio_active()const{return false;}
    virtual bool claim(std::span<const uint8_t>,uint16_t,uint16_t){return false;}
    virtual void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})=0;
    virtual std::vector<RawGipTransmit> poll()=0;
    virtual void complete(uint8_t wcid,uint32_t token,int status)=0;
};
class UsbGipBroker final:public RawGipOutput {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    UsbGipBroker(uint16_t port,Bytes mac);
    ~UsbGipBroker() override;
    bool target(std::span<const uint8_t> mac) const override;
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={}) override;
    std::vector<RawGipTransmit> poll() override;
    void complete(uint8_t wcid,uint32_t token,int status) override;
};
}

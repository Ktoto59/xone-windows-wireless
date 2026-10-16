// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "protocol.hpp"
#include <memory>
#include <chrono>
#include <algorithm>
namespace xone {
using Bytes=std::vector<uint8_t>;
struct RawGipTransmit {
    uint8_t wcid;uint32_t token;Bytes data;bool audio=false;
    std::chrono::steady_clock::time_point media_deadline=std::chrono::steady_clock::time_point::max();
};
struct RawGipFault {uint8_t wcid;std::string error;};
inline void prioritize_control(std::vector<RawGipTransmit> &requests){
    // FIFO within each traffic class, including per-peer round-robin order.
    std::stable_partition(requests.begin(),requests.end(),[](const auto &r){return !r.audio;});
}
class RawGipOutput {
public:
    virtual ~RawGipOutput()=default;
    virtual bool target(std::span<const uint8_t> mac) const=0;
    virtual void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})=0;
    virtual std::vector<RawGipTransmit> poll()=0;
    virtual void complete(uint8_t wcid,uint32_t token,int status)=0;
    virtual std::vector<RawGipFault> faults(){return {};}
    virtual uint64_t failures()const{return 0;}
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

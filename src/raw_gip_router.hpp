// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw_gip.hpp"
#include <algorithm>
#include <stdexcept>
namespace xone {
// One independent USB broker per MAC. WCID ownership is assigned on RF association.
class RawGipRouter final:public RawGipOutput {
    std::vector<std::unique_ptr<RawGipOutput>> outputs_;
    std::array<RawGipOutput *,16> owners_{};
public:
    explicit RawGipRouter(std::vector<std::unique_ptr<RawGipOutput>> outputs):outputs_(std::move(outputs)){
        if(outputs_.empty() || outputs_.size()>16 || std::any_of(outputs_.begin(),outputs_.end(),[](const auto &p){return !p;}))throw std::runtime_error("Invalid USB GIP routes");
    }
    bool target(std::span<const uint8_t> mac)const override{
        return std::any_of(outputs_.begin(),outputs_.end(),[&](const auto &p){return p->target(mac);});
    }
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})override{
        if(!wcid || wcid>16)throw std::runtime_error("Invalid routed USB GIP WCID");
        auto &owner=owners_[wcid-1];
        if(kind==1){
            if(data.size()!=6 || owner)throw std::runtime_error("USB GIP association conflict");
            RawGipOutput *selected=nullptr;
            for(const auto &p:outputs_)if(p->target(data)){
                if(selected)throw std::runtime_error("Overlapping USB GIP routes");selected=p.get();
            }
            if(!selected)throw std::runtime_error("Missing USB GIP route");
            selected->event(kind,wcid,data);owner=selected;
        }else{
            if(!owner)throw std::runtime_error("USB GIP event has no owner");
            owner->event(kind,wcid,data);if(kind==3)owner=nullptr;
        }
    }
    std::vector<RawGipTransmit> poll()override{
        std::vector<RawGipTransmit> requests;
        for(const auto &p:outputs_)for(auto &request:p->poll()){
            if(!request.wcid || request.wcid>16 || owners_[request.wcid-1]!=p.get()){
                p->complete(request.wcid,request.token,-19);continue;
            }
            requests.push_back(std::move(request));
        }
        return requests;
    }
    void complete(uint8_t wcid,uint32_t token,int status)override{
        if(wcid && wcid<=16 && owners_[wcid-1])owners_[wcid-1]->complete(wcid,token,status);
    }
};
}

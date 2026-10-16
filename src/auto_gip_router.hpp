// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw_gip.hpp"
#include <algorithm>
#include <functional>
#include <iostream>

namespace xone {
// Every association owns a fresh TCP connection. Closing that connection on
// removal discards its pending requests, including when a WCID/slot is reused.
// Listeners are cheap; the supervisor starts an isolated worker only on accept.
class AutoGipRouter final:public RawGipOutput {
public:
    using Factory=std::function<std::unique_ptr<RawGipOutput>(size_t,const Bytes &)>;
private:
    struct Slot{std::unique_ptr<RawGipOutput> peer;uint8_t wcid=0;};
    std::vector<Slot> slots_;
    std::array<int,16> owners_{};
    Factory factory_;
    size_t first_=0;
    uint64_t failures_=0;
    std::vector<RawGipFault> faults_;
    void release(size_t n){owners_[slots_[n].wcid-1]=-1;slots_[n].peer.reset();slots_[n].wcid=0;}
    void failed(uint8_t wcid,std::string error){
        ++failures_;
        if(std::none_of(faults_.begin(),faults_.end(),[&](const auto &f){return f.wcid==wcid;}))faults_.push_back({wcid,std::move(error)});
    }
public:
    AutoGipRouter(size_t count,Factory factory):slots_(count),factory_(std::move(factory)){
        if(!count || count>16 || !factory_)throw std::runtime_error("Invalid automatic USB slot pool");
        owners_.fill(-1);
    }
    bool target(std::span<const uint8_t> mac)const override{return mac.size()==6;}
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})override{
        if(!wcid || wcid>16)throw std::runtime_error("Invalid automatic WCID");
        auto &owner=owners_[wcid-1];
        if(kind==3)std::erase_if(faults_,[&](const auto &f){return f.wcid==wcid;});
        if(kind==1){
            if(data.size()!=6 || owner!=-1)throw std::runtime_error("Automatic association conflict");
            const auto available=std::find_if(slots_.begin(),slots_.end(),[](const auto &s){return !s.peer;});
            if(available==slots_.end()){failed(wcid,"Automatic USB slot pool exhausted");return;}
            const auto n=static_cast<size_t>(available-slots_.begin());
            try{
                auto peer=factory_(n,Bytes(data.begin(),data.end()));
                if(!peer || !peer->target(data))throw std::runtime_error("Automatic peer identity mismatch");
                peer->event(kind,wcid,data);available->peer=std::move(peer);available->wcid=wcid;owner=static_cast<int>(n);
            }catch(const std::exception &e){failed(wcid,e.what());}
        }else if(owner!=-1){
            const auto n=static_cast<size_t>(owner);
            if(kind==3){release(n);return;} // TCP EOF resets only this worker.
            try{slots_[n].peer->event(kind,wcid,data);}
            catch(const std::exception &e){failed(wcid,e.what());release(n);}
        }
    }
    std::vector<RawGipTransmit> poll()override{
        std::vector<std::vector<RawGipTransmit>> batches(slots_.size());size_t longest=0;
        for(size_t at=0;at<slots_.size();++at){
            const auto n=(first_+at)%slots_.size();auto &s=slots_[n];if(!s.peer)continue;
            try{
                for(auto &request:s.peer->poll()){
                    if(request.wcid!=s.wcid){s.peer->complete(request.wcid,request.token,-19);continue;}
                    batches[n].push_back(std::move(request));
                }
                longest=std::max(longest,batches[n].size());
            }catch(const std::exception &e){
                failed(s.wcid,e.what());release(n);batches[n].clear();
            }
        }
        std::vector<RawGipTransmit> out;
        for(size_t i=0;i<longest;++i)for(size_t at=0;at<slots_.size();++at){auto &b=batches[(first_+at)%slots_.size()];if(i<b.size())out.push_back(std::move(b[i]));}
        if(!out.empty())first_=(first_+1)%slots_.size();return out;
    }
    void complete(uint8_t wcid,uint32_t token,int status)override{
        if(wcid && wcid<=16 && owners_[wcid-1]!=-1){const auto n=static_cast<size_t>(owners_[wcid-1]);
            try{slots_[n].peer->complete(wcid,token,status);}catch(const std::exception &e){failed(wcid,e.what());release(n);}
        }
    }
    std::vector<RawGipFault> faults()override{auto result=std::move(faults_);faults_.clear();return result;}
    size_t capacity()const{return slots_.size();}
    size_t active()const{return static_cast<size_t>(std::count_if(slots_.begin(),slots_.end(),[](const auto &s){return bool(s.peer);}));}
    uint64_t failures()const override{return failures_;}
};
}

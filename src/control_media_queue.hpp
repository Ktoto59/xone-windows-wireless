// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <utility>

namespace xone {
// External synchronization belongs to the owner. Reliable traffic has its own
// capacity and FIFO; only media can expire or be evicted under pressure.
template<class T> class ControlMediaQueue {
public:
    using Clock=std::chrono::steady_clock;
private:
    struct Entry{T value;uint8_t peer;Clock::time_point arrived;};
    std::deque<Entry> reliable_,media_;
    size_t reliable_limit_,media_limit_,per_peer_;
    uint64_t dropped_=0;
    size_t high_water_=0;
    void expire(Clock::time_point now){
        std::erase_if(media_,[&](const auto &e){if(now-e.arrived>std::chrono::milliseconds(32)){++dropped_;return true;}return false;});
    }
public:
    ControlMediaQueue(size_t reliable_limit=4096,size_t media_limit=64,size_t per_peer=4)
        :reliable_limit_(reliable_limit),media_limit_(media_limit),per_peer_(per_peer){
        if(!reliable_limit || !media_limit || !per_peer)throw std::invalid_argument("Invalid traffic queue limits");
    }
    void push(T value,bool media,uint8_t peer,Clock::time_point now=Clock::now()){
        expire(now);
        if(!media){
            if(reliable_.size()>=reliable_limit_)throw std::overflow_error("Reliable control queue overflow");
            reliable_.push_back({std::move(value),peer,now});high_water_=std::max(high_water_,reliable_.size());return;
        }
        while(static_cast<size_t>(std::count_if(media_.begin(),media_.end(),[&](const auto &e){return e.peer==peer;}))>=per_peer_){
            media_.erase(std::find_if(media_.begin(),media_.end(),[&](const auto &e){return e.peer==peer;}));++dropped_;
        }
        if(media_.size()>=media_limit_){media_.pop_front();++dropped_;}
        media_.push_back({std::move(value),peer,now});
    }
    template<class Predicate> void discard_media(Predicate predicate){
        // Normal lifecycle invalidation is not a queue-pressure anomaly.
        std::erase_if(media_,[&](const auto &e){return predicate(e.value);});
    }
    template<class Predicate> bool any_reliable(Predicate predicate)const{
        return std::any_of(reliable_.begin(),reliable_.end(),[&](const auto &e){return predicate(e.value);});
    }
    std::deque<T> drain(Clock::time_point now=Clock::now(),size_t budget=64){
        expire(now);std::deque<T> result;
        while(!reliable_.empty() && result.size()<budget){result.push_back(std::move(reliable_.front().value));reliable_.pop_front();}
        // Do not let media overtake a queued control/lifecycle barrier.
        if(reliable_.empty())while(!media_.empty() && result.size()<budget){result.push_back(std::move(media_.front().value));media_.pop_front();}
        return result;
    }
    bool empty()const{return reliable_.empty() && media_.empty();}
    size_t size()const{return reliable_.size()+media_.size();}
    size_t reliable_size()const{return reliable_.size();}
    size_t media_size()const{return media_.size();}
    size_t reliable_high_water()const{return high_water_;}
    uint64_t dropped_media()const{return dropped_;}
};
}

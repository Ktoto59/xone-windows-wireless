// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <stdexcept>
#include <vector>

namespace xone {
class RadioIpcQueue {
public:
    using Clock=std::chrono::steady_clock;
private:
    struct Entry{std::vector<uint8_t> bytes;bool audio;Clock::time_point arrived;};
    std::deque<Entry> queue_;
    size_t sent_=0;
    uint64_t dropped_=0;
    void expire(Clock::time_point now){
        for(auto it=queue_.begin();it!=queue_.end();){
            // TCP frames already partially written must always finish intact.
            if(it->audio && !(it==queue_.begin() && sent_) && now-it->arrived>std::chrono::milliseconds(32)){
                it=queue_.erase(it);++dropped_;
            }else ++it;
        }
    }
    bool drop_audio(){
        auto it=std::find_if(queue_.begin(),queue_.end(),[&](const Entry &e){return e.audio && !(&e==&queue_.front() && sent_);});
        if(it==queue_.end())return false;
        queue_.erase(it);++dropped_;return true;
    }
public:
    void push(std::vector<uint8_t> bytes,bool audio,Clock::time_point now=Clock::now()){
        expire(now);
        if(audio){
            while(std::count_if(queue_.begin(),queue_.end(),[](const Entry &e){return e.audio;})>=4){
                if(!drop_audio()){++dropped_;return;}
            }
        }
        if(!audio && std::count_if(queue_.begin(),queue_.end(),[](const auto &e){return !e.audio;})>=128){
            throw std::runtime_error("USB GIP reliable queue limit");
        }
        auto at=queue_.end();
        if(!audio){
            at=queue_.begin();if(sent_)++at;
            while(at!=queue_.end() && !at->audio)++at;
        }
        queue_.insert(at,{std::move(bytes),audio,now});
    }
    std::span<const uint8_t> next(Clock::time_point now=Clock::now()){
        expire(now);
        if(queue_.empty())return {};
        return std::span<const uint8_t>(queue_.front().bytes).subspan(sent_);
    }
    void advance(size_t bytes){
        if(queue_.empty() || bytes>queue_.front().bytes.size()-sent_)throw std::logic_error("Invalid radio IPC send count");
        sent_+=bytes;
        if(sent_==queue_.front().bytes.size()){queue_.pop_front();sent_=0;}
    }
    size_t size() const{return queue_.size();}
    uint64_t dropped_audio() const{return dropped_;}
};
}

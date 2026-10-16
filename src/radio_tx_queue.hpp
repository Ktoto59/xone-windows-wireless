// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw_gip.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace xone {
// One physical OUT operation at a time. Success is delivered only after the
// sender completes. Bounded outstanding work includes unconsumed completions.
class RadioTxQueue {
public:
    using Clock=std::chrono::steady_clock;
    using Sender=std::function<int(std::span<const uint8_t>,std::stop_token,Clock::time_point)>;
    struct Work {uint8_t wcid;uint32_t token;uint64_t epoch;bool audio;Clock::time_point deadline;std::vector<Bytes> packets;Clock::time_point queued=Clock::now();std::stop_token cancellation;};
    struct Result {uint8_t wcid;uint32_t token;uint64_t epoch;bool audio;int status;uint64_t packets=0,over_8ms=0;int64_t last_us=0,max_us=0,queue_us=0;};
private:
    Sender sender_;std::mutex mutex_;std::condition_variable wake_;
    std::deque<Work> control_,media_;std::vector<Result> done_;
    size_t controls_=0,medias_=0;bool closing_=false;
    uint8_t active_wcid_=0;uint64_t active_epoch_=0;std::stop_source active_stop_;
    std::jthread worker_;
    static Result result(const Work &w,int status){return {w.wcid,w.token,w.epoch,w.audio,status};}
    void run(){
        for(;;){
            Work work{};std::stop_token stop;std::stop_source active;
            {std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return closing_ || !control_.empty() || !media_.empty();});
                if(closing_){for(const auto &w:control_)done_.push_back(result(w,-19));for(const auto &w:media_)done_.push_back(result(w,-19));control_.clear();media_.clear();return;}
                auto &queue=control_.empty()?media_:control_;work=std::move(queue.front());queue.pop_front();
                active_wcid_=work.wcid;active_epoch_=work.epoch;active_stop_=std::stop_source{};active=active_stop_;stop=active.get_token();}
            auto completed=result(work,0);
            completed.queue_us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-work.queued).count();
            try{
                std::stop_callback cancelled(work.cancellation,[active]()mutable{active.request_stop();});
                for(const auto &packet:work.packets){
                    if(stop.stop_requested()){completed.status=-19;break;}
                    if(work.audio && Clock::now()>work.deadline){completed.status=-18;break;}
                    const auto before=Clock::now();completed.status=sender_(packet,stop,work.audio?work.deadline:Clock::time_point::max());
                    completed.last_us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-before).count();
                    completed.max_us=std::max(completed.max_us,completed.last_us);
                    if(completed.status)break;
                    ++completed.packets;if(completed.last_us>8000)++completed.over_8ms;
                }
            }catch(...){completed.status=-19;}
            {std::lock_guard lock(mutex_);active_wcid_=0;done_.push_back(std::move(completed));}
        }
    }
public:
    explicit RadioTxQueue(Sender sender):sender_(std::move(sender)){
        if(!sender_)throw std::invalid_argument("Missing radio sender");worker_=std::jthread([this]{run();});
    }
    ~RadioTxQueue(){stop();}
    bool push(Work work){
        if(!work.wcid || work.wcid>16 || work.packets.empty() || work.packets.size()>8 || std::any_of(work.packets.begin(),work.packets.end(),[](const auto &p){return p.empty() || p.size()>4096;}))throw std::invalid_argument("Invalid radio work");
        std::lock_guard lock(mutex_);
        if(closing_)return false;
        if(work.audio){
            if(medias_>=64 || std::count_if(media_.begin(),media_.end(),[&](const auto &w){return w.wcid==work.wcid;})>=4)return false;
            ++medias_;media_.push_back(std::move(work));
        }else{if(controls_>=256)throw std::overflow_error("Reliable radio TX queue overflow");++controls_;control_.push_back(std::move(work));}
        wake_.notify_one();return true;
    }
    std::vector<Result> poll(){
        std::lock_guard lock(mutex_);auto result=std::move(done_);done_.clear();
        for(const auto &r:result)if(r.audio)--medias_;else --controls_;return result;
    }
    void cancel(uint8_t wcid,uint64_t epoch){
        std::optional<std::stop_source> stop;
        {std::lock_guard lock(mutex_);
            auto discard=[&](auto &queue){std::erase_if(queue,[&](const auto &w){if(w.wcid!=wcid || w.epoch!=epoch)return false;done_.push_back(result(w,-19));return true;});};
            discard(control_);discard(media_);if(active_wcid_==wcid && active_epoch_==epoch)stop=active_stop_;}
        if(stop)stop->request_stop();
    }
    void stop(){
        std::stop_source stop;
        {std::lock_guard lock(mutex_);closing_=true;stop=active_stop_;}
        stop.request_stop();wake_.notify_one();if(worker_.joinable())worker_.join();
    }
};
}

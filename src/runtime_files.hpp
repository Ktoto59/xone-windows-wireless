// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace xone {
// File access (including OneDrive/antivirus delays) never runs on radio tick.
// One pending snapshot, overwritten by the newest; no unbounded work queue.
class RuntimeFiles {
    std::function<void(const std::string &)> write_;
    std::function<bool()> stop_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> pending_;
    bool closing_=false;
    std::atomic_bool stopped_=false;
    std::atomic_uint64_t errors_=0;
    std::thread worker_;
    void run() noexcept {
        for(;;){
            std::optional<std::string> text;
            {
                std::unique_lock lock(mutex_);
                changed_.wait_for(lock,std::chrono::milliseconds(100),[&]{return closing_ || pending_.has_value();});
                text=std::move(pending_);pending_.reset();
                if(closing_ && !text)return;
            }
            try{if(text && write_)write_(*text);}catch(...){++errors_;}
            try{if(stop_ && stop_())stopped_=true;}catch(...){++errors_;}
        }
    }
public:
    RuntimeFiles(std::function<void(const std::string &)> write,std::function<bool()> stop)
        :write_(std::move(write)),stop_(std::move(stop)),worker_([this]{run();}){}
    ~RuntimeFiles(){
        {std::lock_guard lock(mutex_);closing_=true;}
        changed_.notify_one();worker_.join();
    }
    void publish(std::string text){
        {std::lock_guard lock(mutex_);pending_=std::move(text);}
        changed_.notify_one();
    }
    bool stop_requested() const{return stopped_.load();}
    uint64_t io_errors() const{return errors_.load();}
};
}

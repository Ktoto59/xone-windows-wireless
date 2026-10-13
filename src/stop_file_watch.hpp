// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
namespace xone {
class StopFileWatch {
    std::atomic_bool requested_=false;
    std::mutex mutex_;std::condition_variable wake_;std::jthread thread_;
public:
    explicit StopFileWatch(std::filesystem::path path){
        if(path.empty())return;
        thread_=std::jthread([this,path=std::move(path)](std::stop_token stop){
            std::unique_lock lock(mutex_);
            while(!stop.stop_requested()){
                lock.unlock();std::error_code error;
                const bool exists=std::filesystem::exists(path,error);
                if(!error && exists){requested_=true;return;}
                lock.lock();wake_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stop.stop_requested();});
            }
        });
    }
    ~StopFileWatch(){thread_.request_stop();wake_.notify_one();if(thread_.joinable())thread_.join();}
    bool requested()const{return requested_.load();}
};
}

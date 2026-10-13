// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace xone {
// One pending snapshot: slow observational IO cannot stall radio or grow memory.
class LatestSnapshot {
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::string> pending_;
    bool stopping_=false;
    std::function<void(const std::string &)> write_;
    std::thread worker_;
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock,[this]{return stopping_ || pending_.has_value();});
            if(!pending_)return;
            auto text=std::move(*pending_);pending_.reset();lock.unlock();
            try {write_(text);}catch (...) {} // Status IO never terminates devices.
            lock.lock();
        }
    }
public:
    explicit LatestSnapshot(std::function<void(const std::string &)> write):write_(std::move(write)),worker_([this]{run();}) {}
    ~LatestSnapshot(){ {std::lock_guard lock(mutex_);stopping_=true;}wake_.notify_one();worker_.join(); }
    LatestSnapshot(const LatestSnapshot &)=delete;
    LatestSnapshot &operator=(const LatestSnapshot &)=delete;
    void publish(std::string text){ {std::lock_guard lock(mutex_);pending_=std::move(text);}wake_.notify_one(); }
};
}

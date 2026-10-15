// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace xone {
// Lifecycle/error changes bypass the cadence used for observational counters.
class StatusSchedule {
public:
    using Clock=std::chrono::steady_clock;
    using Revision=std::pair<uint64_t,uint64_t>;
private:
    std::chrono::milliseconds interval_;
    std::optional<Revision> previous_;
    Clock::time_point last_{};
public:
    explicit StatusSchedule(std::chrono::milliseconds interval=std::chrono::seconds(2)):interval_(interval){}
    bool ready(Revision revision,Clock::time_point now=Clock::now(),bool force=false){
        if(!force && previous_==revision && now-last_<interval_)return false;
        previous_=revision;last_=now;return true;
    }
};
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace xone {
// One lifetime for the host, including USB disappear/reappear and power events.
// Adapter enumeration must never reset the retry deadline or failure count.
class RadioRestartBackoff {
public:
    using Clock=std::chrono::steady_clock;
private:
    unsigned failures_=0;
    Clock::time_point retry_at_{};
    std::optional<Clock::time_point> running_since_;
public:
    unsigned failures()const{return failures_;}
    std::chrono::seconds delay()const{
        constexpr unsigned seconds[]={0,4,8,16,30,60};
        return std::chrono::seconds(seconds[std::min(failures_,5u)]);
    }
    bool ready(Clock::time_point now)const{return now>=retry_at_;}
    void stopped(){running_since_.reset();}
    void failed(Clock::time_point now){
        stopped();if(failures_<std::numeric_limits<unsigned>::max())++failures_;
        retry_at_=std::max(retry_at_,now+delay());
    }
    void started(Clock::time_point now){running_since_=now;}
    bool observe_running(Clock::time_point now){
        if(!failures_ || !running_since_ || now-*running_since_<std::chrono::seconds(60))return false;
        failures_=0;return true;
    }
};
}

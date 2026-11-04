// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <optional>
namespace xone {
class PeerRecoveryBackoff {
public:
    using Clock=std::chrono::steady_clock;
private:
    unsigned failures_=0;
    Clock::time_point retry_at_{};
    std::optional<Clock::time_point> stable_since_;
public:
    unsigned failures()const{return failures_;}
    std::chrono::seconds delay()const{return std::chrono::seconds(failures_?std::min(1u<<(failures_-1),60u):0);}
    bool ready(Clock::time_point now)const{return now>=retry_at_;}
    void interrupted(){stable_since_.reset();}
    void failed(Clock::time_point now){interrupted();failures_=std::min(failures_+1,7u);retry_at_=std::max(retry_at_,now+delay());}
    bool observe_healthy(bool healthy,Clock::time_point now){
        if(!healthy){interrupted();return false;}
        if(!stable_since_)stable_since_=now;
        if(!failures_ || now-*stable_since_<std::chrono::seconds(60))return false;
        failures_=0;return true;
    }
};
}

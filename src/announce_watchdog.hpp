// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
namespace xone {
class AnnounceWatchdog {
public:
    using Clock=std::chrono::steady_clock;
private:
    bool waiting_=false;Clock::time_point deadline_{};
public:
    void arm(Clock::time_point now){waiting_=true;deadline_=now+std::chrono::seconds(5);}
    void clear(){waiting_=false;}
    bool expired(Clock::time_point now)const{return waiting_ && now>=deadline_;}
};
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <functional>
#include <cstdint>

namespace xone {
// Called only by the session thread. Pending IN reads remain posted during
// standby; a wireless event wakes immediately, with a bounded recovery window
// if firmware/USB remote wake fails to deliver an event.
class RadioIdlePower {
public:
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
private:
    std::function<void()> suspend_;
    std::function<void(const char *)> resume_;
    Time deadline_;
    bool armed_=false;
    uint64_t revision_=0;
public:
    RadioIdlePower(Time now,std::function<void()> suspend,std::function<void(const char *)> resume)
        :suspend_(std::move(suspend)),resume_(std::move(resume)),deadline_(now+std::chrono::seconds(30)){}
    bool armed()const{return armed_;}
    uint64_t revision()const{return revision_;}
    void wake(Time now,const char *reason){
        if(armed_){resume_(reason);armed_=false;++revision_;}
        deadline_=now+std::chrono::seconds(30);
    }
    void update(Time now,bool busy){
        if(busy){wake(now,"client-or-pairing");return;}
        if(now<deadline_)return;
        if(armed_){
            wake(now,"watchdog");
            deadline_=now+std::chrono::seconds(2);
        }else{
            suspend_();armed_=true;++revision_;
            deadline_=now+std::chrono::seconds(30);
        }
    }
};
}

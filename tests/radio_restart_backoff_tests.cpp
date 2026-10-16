// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio_restart_backoff.hpp"
#include <iostream>
#include <stdexcept>
using xone::RadioRestartBackoff;using namespace std::chrono_literals;
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int main(){try{
    const auto t=RadioRestartBackoff::Clock::time_point{}+1h;
    RadioRestartBackoff retry;
    require(retry.ready(t) && retry.failures()==0,"first startup should be immediate");
    retry.started(t);retry.failed(t+1s); // USB loss, with the process handle closed by cleanup.
    require(retry.failures()==1 && retry.delay()==4s,"USB loss must count as a startup failure");
    // Re-enumeration notifications and stop/resume cleanup cannot erase the deadline.
    retry.stopped();
    require(!retry.ready(t+1500ms),"rapid USB return bypassed cooldown");
    require(!retry.ready(t+4999ms) && retry.ready(t+5s),"first retry boundary");
    retry.started(t+5s);retry.failed(t+6s);
    require(retry.failures()==2 && retry.delay()==8s && !retry.ready(t+13s),"second flap did not back off");
    require(retry.ready(t+14s),"second retry boundary");
    auto now=t+14s;
    for(auto expected:{16s,30s,60s,60s}){
        retry.started(now);now+=1s;retry.failed(now);
        require(retry.delay()==expected,"retry progression/cap");
        require(!retry.ready(now+expected-1ms) && retry.ready(now+expected),"retry deadline/cap");
        now+=expected;
    }
    // Repeated short-lived processes never accumulate enough uptime to reset failures.
    retry.started(now);require(!retry.observe_running(now+59s),"reset before stable interval");
    retry.stopped();retry.started(now+59s);
    require(!retry.observe_running(now+60s),"uptime incorrectly accumulated across children");
    require(retry.observe_running(now+119s) && retry.failures()==0,"stable run did not reset failures");
    require(!retry.observe_running(now+120s),"stable recovery should be reported only once");
    retry.failed(now+121s);require(retry.delay()==4s,"new incident after stable recovery");
    // Resume without a child does not claim stable runtime or shorten a previous wait.
    retry.stopped();require(!retry.observe_running(now+1h),"stopped child was treated as healthy");
    RadioRestartBackoff repeated;repeated.failed(t);repeated.failed(t+1ms);
    require(!repeated.ready(t+8s) && repeated.ready(t+8001ms),"repeat failure shortened existing wait");
    std::cout<<"PASS radio restart backoff: USB flaps, closed handle, resume, process failures, stable recovery and cap\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

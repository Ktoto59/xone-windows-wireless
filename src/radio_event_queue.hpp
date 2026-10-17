// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "control_media_queue.hpp"
#include "protocol.hpp"
namespace xone {
inline bool radio_audio_only(const RadioEvent &event){
    if(event.kind!=RadioEvent::Kind::gip)return false;
    try{
        const auto packets=parse_gip(event.payload);
        return !packets.empty() && std::all_of(packets.begin(),packets.end(),[](const auto &p){return p.command==0x60;});
    }catch(const std::exception &){return false;} // Unknown/mixed data stays reliable.
}
class RadioEventQueue {
    ControlMediaQueue<RadioEvent> queue_;
public:
    using Clock=ControlMediaQueue<RadioEvent>::Clock;
    void push(RadioEvent event,Clock::time_point now=Clock::now()){
        if(event.kind==RadioEvent::Kind::ignored)return;
        if(event.kind==RadioEvent::Kind::lost || event.kind==RadioEvent::Kind::disassociation)
            queue_.discard_media([&](const auto &e){return e.wcid==event.wcid;});
        if(event.kind==RadioEvent::Kind::association)
            queue_.discard_media([&](const auto &e){return e.address==event.address;});
        const auto media=radio_audio_only(event);const auto wcid=event.wcid;
        queue_.push(std::move(event),media,wcid,now);
    }
    auto drain(Clock::time_point now=Clock::now()){return queue_.drain(now);}
    uint64_t dropped_media()const{return queue_.dropped_media();}
    size_t reliable_high_water()const{return queue_.reliable_high_water();}
};
}

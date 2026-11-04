// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <optional>
namespace xone {
// Small, bounded clock discipline. RX packets are timing observations, not
// receiver grants. Batches/loss gaps must not be learned as a new sample rate.
class RadioRenderTiming {
public:
    using Clock=std::chrono::steady_clock;using Time=Clock::time_point;
    using Duration=std::chrono::nanoseconds;
private:
    Duration period_=std::chrono::milliseconds(8);
    std::optional<Time> previous_;
public:
    Duration period()const{return period_;}
    void clear(){period_=std::chrono::milliseconds(8);previous_.reset();}
    Duration observe(Time arrived,size_t packets,Time next){
        using namespace std::chrono;
        const auto previous=previous_;previous_=arrived;
        if(!previous || arrived<=*previous)return Duration::zero();
        const auto gap=duration_cast<Duration>(arrived-*previous);
        if(gap>=milliseconds(32)){period_=milliseconds(8);return Duration::zero();}
        if(packets!=1 || gap<milliseconds(4) || gap>milliseconds(12))return Duration::zero();
        period_=std::clamp(period_+(gap-period_)/32,Duration(microseconds(7500)),Duration(microseconds(8500)));
        if(next==Time{})return Duration::zero();
        auto phase=duration_cast<Duration>(arrived-next)%period_;
        if(phase>period_/2)phase-=period_;
        if(phase<-period_/2)phase+=period_;
        return std::clamp(phase/8,Duration(-microseconds(100)),Duration(microseconds(100)));
    }
};
}

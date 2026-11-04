// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
namespace xone {
// RX delivery jitter is not a change in the headset sample clock. Estimate
// frequency over packet positions, without moving TX phase on individual RX.
class RadioRenderTiming {
public:
    using Clock=std::chrono::steady_clock;using Time=Clock::time_point;
    using Duration=std::chrono::nanoseconds;
private:
    Duration period_=std::chrono::milliseconds(8);
    std::optional<Time> previous_;
    Time rate_started_{};size_t rate_packets_=0;unsigned rate_samples_=1;
    double sum_position_=0,sum_time_=0,sum_product_=0,sum_square_=0;
    void begin_window(Time arrived){
        rate_started_=arrived;rate_packets_=0;rate_samples_=1;
        sum_position_=sum_time_=sum_product_=sum_square_=0;
    }
public:
    Duration period()const{return period_;}
    void clear(){period_=std::chrono::milliseconds(8);previous_.reset();begin_window({});}
    void observe(Time arrived,size_t packets){
        using namespace std::chrono;
        if(!packets || (previous_ && arrived<*previous_))return;
        const auto previous=previous_;previous_=arrived;
        if(!previous){begin_window(arrived);return;}
        if(arrived-*previous>=milliseconds(32)){period_=milliseconds(8);begin_window(arrived);return;}
        // Include short and coalesced arrivals. Discarding only short intervals
        // biased a lossless 12/1/11 ms stream toward the 8.5 ms upper limit.
        rate_packets_+=packets;++rate_samples_;
        const auto position=static_cast<double>(rate_packets_);
        const auto elapsed=duration<double,std::nano>(arrived-rate_started_).count();
        sum_position_+=position;sum_time_+=elapsed;
        sum_product_+=position*elapsed;sum_square_+=position*position;
        if(rate_packets_<64 || rate_samples_<16)return;
        // A fitted slope avoids turning jitter at the window's endpoints into
        // repeated frequency corrections. The first sample is position/time 0.
        const auto denominator=rate_samples_*sum_square_-sum_position_*sum_position_;
        if(denominator>0){
            const auto slope=(rate_samples_*sum_product_-sum_position_*sum_time_)/denominator;
            if(slope>=7500000 && slope<=8500000)period_+=(Duration(static_cast<int64_t>(slope))-period_)/4;
        }
        begin_window(arrived);
    }
};
}

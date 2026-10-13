// SPDX-License-Identifier: GPL-2.0-or-later
// Negotiation order and wire layout from xone/driver/headset.c and bus/protocol.c.
#pragma once
#include "protocol.hpp"
#include <chrono>
#include <functional>
#include <stdexcept>
namespace xone {
class HeadsetAudio {
public:
    using Clock=std::chrono::steady_clock;
    enum class Stage { config_delay,format_sent,power_delay,waiting_volume,streaming,failed };
private:
    Stage stage_=Stage::config_delay;
    AudioFormat input_,output_;
    Clock::time_point deadline_;
    std::function<void(uint8_t,std::span<const uint8_t>)> send_;
    std::function<void()> power_;
    bool volume_seen_=false;
    unsigned proposals_=0;
    void propose(Clock::time_point now){
        const std::array<uint8_t,3> packet{2,input_.code,output_.code};send_(8,packet);
        ++proposals_;stage_=Stage::format_sent;deadline_=now+std::chrono::seconds(5);
    }
public:
    HeadsetAudio(uint8_t input,uint8_t output,Clock::time_point now,
                 std::function<void(uint8_t,std::span<const uint8_t>)> send,std::function<void()> power)
        :input_(audio_format(input)),output_(audio_format(output)),deadline_(now+std::chrono::seconds(1)),send_(std::move(send)),power_(std::move(power)){}
    Stage stage() const{return stage_;}
    const AudioFormat &input() const{return input_;}
    const AudioFormat &output() const{return output_;}
    const char *stage_name() const{
        switch(stage_){case Stage::config_delay:return "config-delay";case Stage::format_sent:return "format-sent";case Stage::power_delay:return "power-delay";case Stage::waiting_volume:return "waiting-volume";case Stage::streaming:return "streaming-silence";default:return "failed";}
    }
    void tick(Clock::time_point now){
        if(now<deadline_)return;
        if(stage_==Stage::config_delay)propose(now);
        else if(stage_==Stage::format_sent || stage_==Stage::waiting_volume)stage_=Stage::failed;
        else if(stage_==Stage::power_delay){
            const std::array<uint8_t,1> on{0};send_(5,on);power_();
            stage_=volume_seen_?Stage::streaming:Stage::waiting_volume;deadline_=now+std::chrono::seconds(5);
        }
    }
    void receive(std::span<const uint8_t> packet,Clock::time_point now){
        if(packet.empty())throw std::runtime_error("Truncated audio control");
        if(packet[0]==2){
            if(packet.size()!=3)throw std::runtime_error("Invalid audio format response");
            // Ignore duplicate format replies after acceptance; never restart authentication.
            if(stage_!=Stage::format_sent)return;
            if(packet[1]!=input_.code || packet[2]!=output_.code){
                if(proposals_>=3){stage_=Stage::failed;return;}
                try{input_=audio_format(packet[1]);output_=audio_format(packet[2]);}
                catch(const std::exception &){stage_=Stage::failed;return;}
                propose(now);return;
            }
            stage_=Stage::power_delay;deadline_=now+std::chrono::seconds(1);
        }else if(packet[0]==3){
            if(packet.size()!=8)throw std::runtime_error("Invalid audio volume response");
            volume_seen_=true;
            if(stage_==Stage::waiting_volume)stage_=Stage::streaming;
        }
    }
};
}

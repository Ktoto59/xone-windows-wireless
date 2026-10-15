// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "usbip_native_codec.hpp"
#include <chrono>
#include <deque>
#include "device_connector.hpp"
namespace xone::usbip {
// Owned by one USB/audio thread. Contains no sockets, files, sleeps or locks.
class Core {
public:
    using Clock=std::chrono::steady_clock;
    struct Stats {
        uint8_t wcid=0;uint64_t generation=0,rx=0,tx=0,audio_rx=0,audio_tx=0,capture_slots=0,dropped=0;
        size_t capture_high_water=0;bool layout=false;uint64_t render_expired=0,concealed=0,mode_suppressed=0;
        uint64_t power_commands=0;std::string last_power;
    };
private:
    enum class Kind {irq_in,irq_out,capture,render};
    struct Pending {Request request;Kind kind;bool staged=false;uint32_t token=0;Clock::time_point deadline,media_deadline;};
    const Profile &profile_;bool audio_;std::unique_ptr<Control> control_;
    DeviceConnector connector_;
    Stats stats_;std::map<uint32_t,Pending> pending_;std::map<uint32_t,uint32_t> tokens_;
    std::deque<uint32_t> irq_wait_,capture_wait_,render_wait_;
    std::deque<Bytes> irq_,capture_;Bytes pcm_;size_t credits_=0;
    uint32_t next_token_=0;uint8_t capture_sequence_=1,render_sequence_=1;
    Clock::time_point capture_deadline_{};
    Clock::time_point radio_last_{},render_deadline_{},pcm_started_{};
    std::deque<Clock::time_point> capture_times_;
    bool clock_active(Clock::time_point now)const;
    void trim_capture(Clock::time_point now);
    std::vector<Bytes> replies_;std::vector<RawGipTransmit> outbound_;
    void finish(uint32_t seq,Completion result);
    void fail(uint32_t seq,int32_t status);
    bool cancel(uint32_t seq);
    uint32_t transmit(uint32_t seq,Bytes data,bool audio,Clock::time_point now);
    void clear_audio();
    void reset();
    void pump(Clock::time_point now);
public:
    Core(const Profile &profile,bool audio);
    bool ready()const{return stats_.wcid!=0;}
    const Stats &stats()const{return stats_;}
    void imported();
    void detached();
    void submit(Request request,Clock::time_point now=Clock::now());
    void radio(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data,Clock::time_point now=Clock::now());
    void complete(uint8_t wcid,uint32_t token,int32_t status,Clock::time_point now=Clock::now());
    void tick(Clock::time_point now=Clock::now());
    std::optional<Clock::time_point> wake_at()const;
    std::vector<Bytes> replies(){auto result=std::move(replies_);replies_.clear();return result;}
    std::vector<RawGipTransmit> outbound(){auto result=std::move(outbound_);outbound_.clear();return result;}
};
std::vector<Bytes> interrupt_packets(std::span<const uint8_t> data);
}

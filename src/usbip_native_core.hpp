// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "usbip_native_codec.hpp"
#include <chrono>
#include <deque>
#include "device_connector.hpp"
namespace xone::usbip {
// Owned by one USB/audio thread. No transport IO/sleeps; diagnostics enqueue only.
class Core {
public:
    using Clock=std::chrono::steady_clock;
    // Per-import progress, retained after detach so a failed initialization can
    // be inspected without enabling packet logging. No payloads are retained.
    struct Initialization {
        uint64_t configurations=0,interfaces=0,irq_in=0,irq_out=0,irq_delivered=0,capture=0,render=0,commands=0;
    };
    struct Stats {
        uint8_t wcid=0;uint64_t generation=0,rx=0,tx=0,audio_rx=0,audio_tx=0,capture_slots=0,dropped=0;
        size_t capture_high_water=0;bool layout=false;uint64_t render_expired=0,concealed=0,mode_suppressed=0;
        uint64_t power_commands=0;std::string last_power;
        bool recovery_required=false;
        uint64_t usb_imports=0,announce_replays=0,capture_without_usb=0,capture_drop_reports=0,render_expiry_reports=0;
    };
private:
    enum class Kind {irq_in,irq_out,capture,render};
    struct Pending {Request request;Kind kind;bool staged=false;uint32_t token=0;Clock::time_point deadline,media_deadline;};
    const Profile &profile_;bool audio_;std::unique_ptr<Control> control_;
    DeviceConnector connector_;
    Stats stats_;std::map<uint32_t,Pending> pending_;std::map<uint32_t,uint32_t> tokens_;
    Initialization initialization_;
    std::deque<uint32_t> irq_wait_,capture_wait_,render_wait_;
    std::deque<Bytes> irq_,capture_;Bytes pcm_;size_t credits_=0;
    Bytes announce_;
    bool imported_once_=false;
    uint64_t reported_capture_drops_=0,reported_render_expired_=0;
    Clock::time_point capture_reported_at_{},render_reported_at_{};
    uint32_t next_token_=0;uint8_t capture_sequence_=1,render_sequence_=1;
    Clock::time_point capture_deadline_{};
    Clock::time_point radio_last_{},render_deadline_{},pcm_started_{};
    std::deque<Clock::time_point> capture_times_;
    bool clock_active(Clock::time_point now)const;
    void trim_capture(Clock::time_point now);
    void report_media_drops(Clock::time_point now);
    std::vector<Bytes> replies_;std::vector<RawGipTransmit> outbound_;
    void finish(uint32_t seq,Completion result);
    void fail(uint32_t seq,int32_t status);
    void expire_render(uint32_t seq,Clock::time_point now);
    void recover_audio();
    bool cancel(uint32_t seq);
    uint32_t transmit(uint32_t seq,Bytes data,bool audio,Clock::time_point now);
    void clear_audio();
    void reset();
    void pump(Clock::time_point now);
public:
    Core(const Profile &profile,bool audio);
    bool associated()const{return stats_.wcid!=0;}
    bool ready()const{return associated() && !stats_.recovery_required;}
    const Stats &stats()const{return stats_;}
    const Initialization &initialization()const{return initialization_;}
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

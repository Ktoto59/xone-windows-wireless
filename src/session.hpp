// SPDX-License-Identifier: GPL-2.0-or-later
// Port of xone transport/dongle.c, Copyright (C) 2021 Severin von Wnuck-Lipinski.
#pragma once
#include "gip_link.hpp"
#include "auth.hpp"
#include "input_output.hpp"
#include "audio_output.hpp"
#include "raw_gip.hpp"
#include <memory>
#include <chrono>
extern "C" {
#include "mt76/mt76.h"
}
namespace xone {
Bytes wlan_packet(uint8_t wcid,std::span<const uint8_t> client,std::span<const uint8_t> host,bool encrypted,std::span<const uint8_t> gip,bool audio=false);
std::vector<std::string> identification_classes(std::span<const uint8_t> data);
struct ControllerStatus {
    uint8_t wcid;
    std::string mac;
    bool encrypted,share_pressed;
    GamepadCapabilities capabilities;
    std::optional<BatteryStatus> battery;
    std::string kind;
    struct Audio { std::string stage; uint32_t input_rate,output_rate; uint8_t input_channels,output_channels; uint64_t received_packets,silence_packets,playback_packets,capture_packets; AudioEndpoints endpoints; bool synthetic_parent; };
    std::optional<Audio> audio;
    std::string authentication;
    uint8_t auth_version,peer_auth_version;
};
class Session {
    struct Client;
    xone_mt76 &mt_;
    InputOutput *output_;
    AudioOutput *audio_output_;
    RawGipOutput *raw_output_;
    bool trace_;
    std::array<std::unique_ptr<Client>,16> clients_;
    bool pairing_=false;
    std::chrono::steady_clock::time_point pair_deadline_;
    std::chrono::steady_clock::time_point pair_scan_last_;
    size_t pair_scan_index_=0;
    uint64_t inputs_=0,authenticated_=0,rumbles_=0;
    void check(int result,const char *operation);
    void pairing(bool enabled);
    void remove(uint8_t wcid);
public:
    explicit Session(xone_mt76 &mt,InputOutput *output=nullptr,bool trace=true,AudioOutput *audio_output=nullptr,RawGipOutput *raw_output=nullptr);
    ~Session();
    void process(const RadioEvent &event);
    void start_reconnect_window();
    void tick(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    void close();
    uint64_t inputs() const { return inputs_; }
    uint64_t authenticated() const { return authenticated_; }
    uint64_t rumbles() const { return rumbles_; }
    std::vector<ControllerStatus> status() const;
    bool audio_active() const;
    bool pairing_active() const { return pairing_; }
};
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "protocol.hpp"
#include <memory>
namespace xone {
struct AudioEndpoints {std::string render,capture;};
class AudioOutput {
public:
    virtual ~AudioOutput()=default;
    virtual void connect(uint8_t wcid,uint8_t client)=0;
    virtual void disconnect(uint8_t wcid)=0;
    virtual void capture(uint8_t wcid,uint8_t client,std::span<const uint8_t> pcm)=0;
    virtual std::vector<uint8_t> playback(uint8_t wcid,uint8_t client)=0;
    virtual AudioEndpoints endpoints(uint8_t wcid,uint8_t client)=0;
};
class NativeAudioOutput final:public AudioOutput {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    NativeAudioOutput();~NativeAudioOutput();
    void connect(uint8_t wcid,uint8_t client) override;
    void disconnect(uint8_t wcid) override;
    void capture(uint8_t wcid,uint8_t client,std::span<const uint8_t> pcm) override;
    std::vector<uint8_t> playback(uint8_t wcid,uint8_t client) override;
    AudioEndpoints endpoints(uint8_t wcid,uint8_t client) override;
};
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "protocol.hpp"
#include <array>
#include <algorithm>
#include <stdexcept>
namespace xone {
constexpr size_t audio_message_size=1600;
using AudioMessage=std::array<uint8_t,audio_message_size>;
inline AudioMessage audio_message(uint8_t kind,std::span<const uint8_t> payload={},uint8_t flag=0){
    if(payload.size()>audio_message_size-4)throw std::runtime_error("Audio IPC payload too large");
    AudioMessage message{};message[0]=kind;message[1]=flag;message[2]=static_cast<uint8_t>(payload.size());message[3]=static_cast<uint8_t>(payload.size()>>8);
    std::copy(payload.begin(),payload.end(),message.begin()+4);return message;
}
inline std::span<const uint8_t> audio_payload(const AudioMessage &message){
    const size_t length=message[2]|(size_t(message[3])<<8);
    if(length>audio_message_size-4)throw std::runtime_error("Invalid audio IPC length");
    return std::span(message).subspan(4,length);
}
}

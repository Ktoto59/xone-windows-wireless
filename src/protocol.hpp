// SPDX-License-Identifier: GPL-2.0-or-later
// GIP and MT76 wire layout based on medusalix/xone. Original copyright:
// Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
#pragma once
#include <span>
#include <vector>
#include <cstdint>
#include <string>
#include <array>
namespace xone {
struct FirmwareInfo { uint32_t ilm, dlm; uint16_t build, version; std::string timestamp; };
FirmwareInfo validate_firmware(std::span<const uint8_t> bytes);
struct GipPacket {
    uint8_t command, options, sequence;
    uint32_t chunk_offset;
    std::vector<uint8_t> payload;
};
std::vector<GipPacket> parse_gip(std::span<const uint8_t> bytes);
struct GamepadInput {
    uint16_t buttons, left_trigger, right_trigger;
    int16_t left_x,left_y,right_x,right_y;
    bool share=false;
};
struct GamepadCapabilities { bool share=false,dli=false; };
struct BatteryStatus { bool connected; uint8_t type,level; };
GamepadInput parse_gamepad(std::span<const uint8_t> bytes,GamepadCapabilities capabilities={});
GamepadCapabilities identification_capabilities(std::span<const uint8_t> bytes);
std::vector<std::array<uint8_t,16>> identification_interfaces(std::span<const uint8_t> bytes);
BatteryStatus parse_battery_status(std::span<const uint8_t> bytes);
struct AudioFormat { uint8_t code,channels; uint32_t rate,interval_bytes; };
AudioFormat audio_format(uint8_t code);
std::vector<std::array<uint8_t,2>> identification_audio_formats(std::span<const uint8_t> bytes);
struct RadioEvent {
    enum class Kind { ignored, button, lost, association, disassociation, client_command, gip } kind=Kind::ignored;
    uint8_t wcid=0,command=0;
    std::vector<uint8_t> address, payload;
};
RadioEvent parse_radio(std::span<const uint8_t> bytes);
std::string hex(std::span<const uint8_t> bytes);
}

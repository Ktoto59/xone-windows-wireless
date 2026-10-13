// SPDX-License-Identifier: GPL-2.0-or-later
// Based on xone bus/protocol.c, Copyright (C) 2021 Severin von Wnuck-Lipinski.
#pragma once
#include "protocol.hpp"
#include <functional>
#include <optional>
#include <array>
namespace xone {
std::vector<uint8_t> encode_gip(const GipPacket &packet);
class GipLink {
    struct Transfer { GipPacket packet; std::vector<uint8_t> data, received; bool sent=false; };
    std::optional<Transfer> incoming_,outgoing_;
    uint8_t sequence_=1,id_;
    std::function<void(const GipPacket &)> transmit_,dispatch_;
public:
    GipLink(uint8_t id,std::function<void(const GipPacket &)> transmit,std::function<void(const GipPacket &)> dispatch);
    void send(uint8_t command,std::span<const uint8_t> data={},bool acknowledge=false,bool internal=true);
    void receive(GipPacket packet);
};
}

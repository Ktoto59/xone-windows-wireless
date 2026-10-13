// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <span>
#include <cstdint>
namespace xone {
// One 8 ms render block follows one valid microphone frame from the headset.
// Collapse delayed frames to one credit: never burst stale audio after a stall.
class RadioAudioClock {
    bool credit_=false;
public:
    bool receive(std::span<const uint8_t> payload) {
        if(payload.size()!=386 || payload[0]!=0 || payload[1]!=6)return false;
        credit_=true;return true;
    }
    bool take() {const bool ready=credit_;credit_=false;return ready;}
};
}

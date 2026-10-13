// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <span>
#include <cstdint>
namespace xone {
// One 8 ms render block follows one valid microphone frame from the headset.
// Match the working USB bridge's two-credit reserve for batched radio reads.
// Bound delayed credits so a long stall never queues an unbounded catch-up burst.
class RadioAudioClock {
    unsigned credits_=0;
public:
    bool receive(std::span<const uint8_t> payload) {
        if(payload.size()!=386 || payload[0]!=0 || payload[1]!=6)return false;
        if(credits_<2)++credits_;return true;
    }
    bool take() {if(!credits_)return false;--credits_;return true;}
};
}

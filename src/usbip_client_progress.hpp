// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <optional>
namespace xone {
// Only a response already waiting to be written is expected to make progress.
// Idle TCP sockets and pending interrupt IN reads have no liveness deadline.
struct UsbipClientProgress {
    using Clock=std::chrono::steady_clock;
    std::optional<Clock::time_point> blocked;
    void queued(Clock::time_point now){if(!blocked)blocked=now;}
    void sent(Clock::time_point now,bool pending){if(pending)blocked=now;else blocked.reset();}
    bool stalled(Clock::time_point now)const{return blocked && now-*blocked>=std::chrono::seconds(15);}
};
}

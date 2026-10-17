// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
namespace xone {
// Only explicit snapshots are passed here. Missing files never detach a port.
struct UsbImportRecovery {
    using Clock=std::chrono::steady_clock;
    static bool detach(bool ready,bool recovery,uint64_t generation,uint64_t previous,
                       bool imported,bool import_known,Clock::time_point attached,Clock::time_point now){
        return generation!=previous || (!ready && !recovery) ||
            ((recovery || (import_known && !imported)) && now-attached>std::chrono::seconds(2));
    }
    static bool attach(bool ready,bool recovery,bool associated){return ready || (recovery && associated);}
};
}

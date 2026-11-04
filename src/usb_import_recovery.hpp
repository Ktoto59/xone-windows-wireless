// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
namespace xone {
// Only explicit snapshots are passed here. Missing files never detach a port.
struct UsbImportRecovery {
    using Clock=std::chrono::steady_clock;
    uint64_t sequence=0,imports=0,closed=0,association=0,attached_tick=0;
    unsigned absent=0;
    bool seen_import=false;
    Clock::time_point attached{};
    void begin(uint64_t seq,uint64_t import_count,uint64_t close_count,Clock::time_point now,uint64_t association_id=0,uint64_t tick=0){
        sequence=seq;imports=import_count;closed=close_count;association=association_id;attached_tick=tick;absent=0;seen_import=false;attached=now;
    }
    bool detach(bool ready,bool recovery,uint64_t generation,uint64_t previous,
                bool imported,bool import_known,uint64_t seq,uint64_t import_count,uint64_t close_count,Clock::time_point now,uint64_t association_id=0,uint64_t published_tick=0){
        // The legacy composite Generation uses association*100+core_generation.
        // After 100 recoveries it can equal a later association's value.
        if((association_id && association && association_id!=association) || generation!=previous || (!ready && !recovery))return true;
        // Counters alone cannot distinguish a delayed close publication from
        // the previous import. Both processes use Windows uptime (not wall time).
        if(attached_tick && published_tick<=attached_tick)return false;
        if(!seq || seq<=sequence)return false; // repeated/missing snapshot
        sequence=seq;
        if(!import_known){absent=0;return false;}
        if(imported){seen_import=true;imports=import_count;closed=close_count;absent=0;return false;}
        // A completed import followed by TCP close is a definite failure, even
        // when the host never saw the intermediate imported=true publication.
        if(close_count>closed)return true;
        if(seen_import || import_count>imports)return true;
        // attach returned successfully but this live server still has no TCP
        // import. Confirm across distinct publications, allowing publication
        // contention. Missing snapshots and repeated reads are not evidence.
        ++absent;
        return absent>=3 && now-attached>=std::chrono::seconds(10);
    }
    static bool attach(bool ready,bool recovery,bool associated){return ready || (recovery && associated);}
};
}

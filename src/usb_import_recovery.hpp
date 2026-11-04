// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>
namespace xone {
enum class UsbDetachReason {none,rf_disconnected,association_changed,generation_changed,usb_recovery,import_closed,import_missing};
inline bool usb_detach_failed(UsbDetachReason reason){
    return reason==UsbDetachReason::usb_recovery || reason==UsbDetachReason::import_closed || reason==UsbDetachReason::import_missing;
}
inline const char *usb_detach_name(UsbDetachReason reason){
    switch(reason){
    case UsbDetachReason::rf_disconnected:return "rf_disconnected";
    case UsbDetachReason::association_changed:return "association_changed";
    case UsbDetachReason::generation_changed:return "generation_changed";
    case UsbDetachReason::usb_recovery:return "usb_recovery";
    case UsbDetachReason::import_closed:return "import_closed";
    case UsbDetachReason::import_missing:return "import_missing";
    default:return "none";
    }
}
struct UsbImportFailure final:std::runtime_error {
    const UsbDetachReason reason;
    explicit UsbImportFailure(UsbDetachReason cause):std::runtime_error(usb_detach_name(cause)),reason(cause){}
};
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
    UsbDetachReason detach_reason(bool ready,bool recovery,uint64_t generation,uint64_t previous,
                bool imported,bool import_known,uint64_t seq,uint64_t import_count,uint64_t close_count,Clock::time_point now,uint64_t association_id=0,uint64_t published_tick=0){
        // The legacy composite Generation uses association*100+core_generation.
        // After 100 recoveries it can equal a later association's value.
        if(association_id && association && association_id!=association)return UsbDetachReason::association_changed;
        if(!ready && !recovery)return UsbDetachReason::rf_disconnected;
        if(generation!=previous)return recovery?UsbDetachReason::usb_recovery:UsbDetachReason::generation_changed;
        // Counters alone cannot distinguish a delayed close publication from
        // the previous import. Both processes use Windows uptime (not wall time).
        if(attached_tick && published_tick<=attached_tick)return UsbDetachReason::none;
        if(!seq || seq<=sequence)return UsbDetachReason::none; // repeated/missing snapshot
        sequence=seq;
        if(!import_known){absent=0;return UsbDetachReason::none;}
        if(imported){seen_import=true;imports=import_count;closed=close_count;absent=0;return UsbDetachReason::none;}
        // A completed import followed by TCP close is a definite failure, even
        // when the host never saw the intermediate imported=true publication.
        if(close_count>closed)return UsbDetachReason::import_closed;
        if(seen_import || import_count>imports)return UsbDetachReason::import_closed;
        // attach returned successfully but this live server still has no TCP
        // import. Confirm across distinct publications, allowing publication
        // contention. Missing snapshots and repeated reads are not evidence.
        ++absent;
        return absent>=3 && now-attached>=std::chrono::seconds(10)?UsbDetachReason::import_missing:UsbDetachReason::none;
    }
    static bool attach(bool ready,bool recovery,bool associated){return ready || (recovery && associated);}
};
}

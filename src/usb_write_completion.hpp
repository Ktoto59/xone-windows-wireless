// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <stop_token>
namespace xone {
struct UsbTransmitResult {
    int status;uint32_t native_error,transferred;
    UsbTransmitResult(int code=0,uint32_t error=0,uint32_t bytes=0):status(code),native_error(error),transferred(bytes){}
};
struct UsbWriteCompletion {bool success;uint32_t bytes,error;};
inline UsbTransmitResult usb_write_failure(uint32_t error,bool cancelled=false,bool timed_out=false){
    const auto status=cancelled?-125:(timed_out || error==ERROR_SEM_TIMEOUT)?-110:
        (error==ERROR_DEVICE_NOT_CONNECTED || error==ERROR_NO_SUCH_DEVICE)?-19:-5;
    return {status,error};
}
// Wait/cancel/drain are injected so real cancellation races can be checked
// without opening a USB device. Drain is mandatory even after a deadline/stop;
// its return owns the final outcome, never the wait result alone.
template<class Wait,class Cancel,class Drain>
UsbTransmitResult finish_usb_write(size_t expected,std::stop_token stop,
    std::chrono::steady_clock::time_point deadline,Wait wait,Cancel cancel,Drain drain){
    using namespace std::chrono;
    const auto end=std::min(deadline,steady_clock::now()+seconds(1));
    const auto left=std::max(milliseconds::zero(),ceil<milliseconds>(end-steady_clock::now()));
    const auto timeout=static_cast<DWORD>(left.count());
    std::stop_callback on_stop(stop,[&]{cancel();});
    const auto waited=wait(timeout);
    if(waited!=WAIT_OBJECT_0)cancel();
    const auto result=drain();
    // Completion can win the race with CancelIoEx: a proven full write is
    // success even if the wait timed out. Never replay this packet as unsent.
    if(result.success)return {result.bytes==expected?0:-5,0,result.bytes};
    auto failure=usb_write_failure(result.error,stop.stop_requested(),waited==WAIT_TIMEOUT);
    failure.transferred=result.bytes;return failure;
}
}

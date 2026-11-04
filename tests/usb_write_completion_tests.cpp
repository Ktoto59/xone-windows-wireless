// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_write_completion.hpp"
#include <future>
#include <iostream>
#include <thread>
using namespace xone;using namespace std::chrono_literals;
void require(bool ok){if(!ok)throw std::runtime_error("USB write completion regression");}
int main(){try{
    using Clock=std::chrono::steady_clock;
    {
        bool cancelled=false,drained=false;
        const auto result=finish_usb_write(8,{},Clock::now()+5ms,
            [&](DWORD ms){require(ms<=5);return DWORD(WAIT_TIMEOUT);},[&]{cancelled=true;},
            [&]{require(cancelled);drained=true;return UsbWriteCompletion{false,2,ERROR_OPERATION_ABORTED};});
        require(cancelled && drained && result.status==-110 && result.native_error==ERROR_OPERATION_ABORTED && result.transferred==2);
    }
    {
        bool cancelled=false;
        const auto result=finish_usb_write(8,{},Clock::now()+1ms,
            [](DWORD){return DWORD(WAIT_TIMEOUT);},[&]{cancelled=true;},
            [&]{require(cancelled);return UsbWriteCompletion{true,8,0};});
        require(result.status==0 && result.transferred==8); // Completion wins timeout/cancel race.
    }
    for(auto error:{DWORD(ERROR_GEN_FAILURE),DWORD(ERROR_DEVICE_NOT_CONNECTED),DWORD(ERROR_SEM_TIMEOUT)}){
        auto result=finish_usb_write(8,{},Clock::now()+10ms,[](DWORD){return DWORD(WAIT_OBJECT_0);},[]{},
            [&]{return UsbWriteCompletion{false,0,error};});
        require(result.native_error==error && result.status==(error==ERROR_GEN_FAILURE?-5:error==ERROR_DEVICE_NOT_CONNECTED?-19:-110));
    }
    {
        auto result=finish_usb_write(8,{},Clock::time_point::max(),[](DWORD ms){require(ms<=1000);return DWORD(WAIT_OBJECT_0);},[]{},
            []{return UsbWriteCompletion{true,3,0};});
        require(result.status==-5 && result.transferred==3 && result.native_error==0);
    }
    {
        // Real Windows event/stop callback race, without an OVERLAPPED device.
        const auto event=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(event!=nullptr);
        struct Cleanup{HANDLE value;~Cleanup(){CloseHandle(value);}} cleanup{event};
        std::stop_source stop;std::promise<void> entered;auto ready=entered.get_future();
        std::jthread canceller([&]{ready.wait();stop.request_stop();});
        bool drained=false;
        const auto result=finish_usb_write(8,stop.get_token(),Clock::now()+500ms,
            [&](DWORD ms){entered.set_value();return WaitForSingleObject(event,ms);},[&]{SetEvent(event);},
            [&]{require(stop.stop_requested());drained=true;return UsbWriteCompletion{false,0,ERROR_OPERATION_ABORTED};});
        require(drained && result.status==-125 && result.native_error==ERROR_OPERATION_ABORTED);
    }
    std::cout<<"PASS: deadline cancellation/drain, completion race, stop event, native errors and short writes without USB\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

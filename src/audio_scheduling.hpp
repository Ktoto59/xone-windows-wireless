// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <avrt.h>
#include <stdexcept>
namespace xone {
class AudioScheduling {
    HANDLE task_=nullptr;
public:
    explicit AudioScheduling(const wchar_t *category=L"Pro Audio") {
        DWORD index=0;task_=AvSetMmThreadCharacteristicsW(category,&index);
        if(task_)AvSetMmThreadPriority(task_,AVRT_PRIORITY_HIGH);
    }
    ~AudioScheduling(){if(task_)AvRevertMmThreadCharacteristics(task_);}
    AudioScheduling(const AudioScheduling &)=delete;
    AudioScheduling &operator=(const AudioScheduling &)=delete;
};
class AudioTick {
    HANDLE timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
public:
    AudioTick(){if(!timer_)throw std::runtime_error("Cannot create native audio timer");}
    ~AudioTick(){CloseHandle(timer_);}
    void wait(){LARGE_INTEGER due{};due.QuadPart=-10000;if(!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE) || WaitForSingleObject(timer_,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("Native audio timer failed");}
    AudioTick(const AudioTick &)=delete;
    AudioTick &operator=(const AudioTick &)=delete;
};
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "appliance_config.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <cwctype>
#include <cstring>
#include <optional>
#include <array>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>

namespace xone {
// Session-local, pagefile-backed IPC. No application file or filesystem polling.
// Producers never wait on a consumer: a busy snapshot is retried by its owner.
class RuntimeState {
    static constexpr DWORD magic=0x584f4e32;
    static constexpr size_t capacity=65536,cells=19;
    struct Cell {DWORD length;char text[capacity];};
    struct Layout {DWORD signature;Cell cell[cells];};
    HANDLE mapping_=nullptr,mutex_=nullptr,changed_=nullptr,stop_=nullptr,radio_stop_=nullptr;
    Layout *view_=nullptr;
    struct Pending {std::string text;bool notify=false;};
    std::mutex pending_mutex_;std::condition_variable pending_wake_;std::array<std::optional<Pending>,cells> pending_;
    bool closing_=false;std::once_flag writer_once_;std::thread writer_;
    bool commit(size_t cell,const std::string &text,bool notify){
        if(!lock())return false;
        {Unlock unlock{mutex_};auto &value=view_->cell[cell];std::memcpy(value.text,text.data(),text.size());value.length=static_cast<DWORD>(text.size());}
        if(notify)SetEvent(changed_);return true;
    }
    void run_writer(){
        std::optional<std::chrono::steady_clock::time_point> deadline;
        for(;;){
            std::array<std::optional<Pending>,cells> batch;bool closing;
            {std::unique_lock lock(pending_mutex_);
                pending_wake_.wait(lock,[&]{return closing_ || std::any_of(pending_.begin(),pending_.end(),[](const auto &p){return p.has_value();});});
                batch.swap(pending_);closing=closing_;}
            if(closing && !deadline)deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            bool retry=false;
            for(size_t n=0;n<cells;++n)if(batch[n]){
                bool written=false;try{written=commit(n,batch[n]->text,batch[n]->notify);}catch(...){}
                if(!written){std::lock_guard lock(pending_mutex_);if(!pending_[n])pending_[n]=std::move(batch[n]);else pending_[n]->notify|=batch[n]->notify;retry=true;}
            }
            {std::lock_guard lock(pending_mutex_);if(closing && std::none_of(pending_.begin(),pending_.end(),[](const auto &p){return p.has_value();}))return;}
            if(deadline && std::chrono::steady_clock::now()>=*deadline)return;
            if(retry)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    static HANDLE required(HANDLE value){if(!value)throw std::runtime_error("Cannot open runtime IPC");return value;}
    void release()noexcept{
        if(view_)UnmapViewOfFile(view_);view_=nullptr;
        for(auto h:{mapping_,mutex_,changed_,stop_,radio_stop_})if(h)CloseHandle(h);
        mapping_=mutex_=changed_=stop_=radio_stop_=nullptr;
    }
    bool lock(DWORD wait=0)const{
        const auto result=WaitForSingleObject(mutex_,wait);
        if(result==WAIT_ABANDONED){ReleaseMutex(mutex_);throw std::runtime_error("Runtime IPC writer abandoned its snapshot");}
        return result==WAIT_OBJECT_0;
    }
    struct Unlock {HANDLE mutex;~Unlock(){ReleaseMutex(mutex);}};
public:
    static constexpr size_t host=16,running=17,radio=18;
    static std::wstring name(const std::filesystem::path &root){
        uint64_t hash=1469598103934665603ULL;
        auto normalized=std::filesystem::absolute(root).lexically_normal().wstring();
        while(normalized.size()>3 && (normalized.back()==L'\\' || normalized.back()==L'/'))normalized.pop_back();
        for(auto c:normalized){if(c==L'/')c=L'\\';hash^=static_cast<uint64_t>(std::towlower(c));hash*=1099511628211ULL;}
        return L"Local\\xone-runtime-v1-"+std::to_wstring(hash);
    }
    RuntimeState(const std::filesystem::path &root,bool owner){
        const auto base=name(root);
        try{
            mutex_=required(owner?CreateMutexW(nullptr,FALSE,(base+L"-lock").c_str()):OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(base+L"-lock").c_str()));
            changed_=required(owner?CreateEventW(nullptr,FALSE,FALSE,(base+L"-changed").c_str()):OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,(base+L"-changed").c_str()));
            stop_=required(owner?CreateEventW(nullptr,TRUE,FALSE,(base+L"-stop").c_str()):OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,(base+L"-stop").c_str()));
            radio_stop_=required(owner?CreateEventW(nullptr,TRUE,FALSE,(base+L"-radio-stop").c_str()):OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,(base+L"-radio-stop").c_str()));
            mapping_=required(owner?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Layout)),(base+L"-data").c_str()):OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,(base+L"-data").c_str()));
            view_=static_cast<Layout *>(MapViewOfFile(mapping_,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Layout)));
            if(!view_)throw std::runtime_error("Cannot map runtime IPC");
            if(owner){
                if(!lock())throw std::runtime_error("Runtime IPC is busy during startup");
                std::memset(view_,0,sizeof(Layout));view_->signature=magic;ReleaseMutex(mutex_);
                ResetEvent(stop_);ResetEvent(radio_stop_);ResetEvent(changed_);
            }else if(view_->signature!=magic)throw std::runtime_error("Unsupported runtime IPC");
        }catch(...){release();throw;}
    }
    ~RuntimeState(){
        {std::lock_guard lock(pending_mutex_);closing_=true;}pending_wake_.notify_one();
        if(writer_.joinable())writer_.join();release();
    }
    RuntimeState(const RuntimeState &)=delete;RuntimeState &operator=(const RuntimeState &)=delete;
    bool publish(size_t cell,const std::string &text,bool notify=false){
        if(cell>=cells || text.size()>=capacity)throw std::runtime_error("Runtime snapshot exceeds bounds");
        std::call_once(writer_once_,[this]{writer_=std::thread([this]{run_writer();});});
        {std::lock_guard lock(pending_mutex_);if(closing_)return false;
            const bool signal=notify || (pending_[cell] && pending_[cell]->notify);pending_[cell]=Pending{text,signal};}
        pending_wake_.notify_one();return true;
    }
    Json read(size_t cell,DWORD wait=0)const{
        if(cell>=cells)throw std::runtime_error("Invalid runtime snapshot cell");
        if(!lock(wait))return Json::object();
        std::string text;
        {Unlock unlock{mutex_};const auto &value=view_->cell[cell];
            if(value.length>=capacity)throw std::runtime_error("Invalid runtime snapshot size");
            text.assign(value.text,value.length);}
        auto result=Json::parse(text,nullptr,false);return result.is_object()?result:Json::object();
    }
    void clear_radio(){
        ResetEvent(radio_stop_);
        for(size_t n=0;n<16;++n)if(!commit(n,"{}",false))throw std::runtime_error("Cannot reset runtime slot");
        if(!commit(radio,"{}",false))throw std::runtime_error("Cannot reset radio snapshot");
    }
    HANDLE changed_event()const{return changed_;}
    HANDLE stop_event()const{return stop_;}
    HANDLE radio_stop_event()const{return radio_stop_;}
    bool stopped()const{return WaitForSingleObject(stop_,0)==WAIT_OBJECT_0;}
    bool radio_stopped()const{return stopped() || WaitForSingleObject(radio_stop_,0)==WAIT_OBJECT_0;}
    void request_stop(){SetEvent(stop_);}
    void request_radio_stop(){SetEvent(radio_stop_);}
};
}

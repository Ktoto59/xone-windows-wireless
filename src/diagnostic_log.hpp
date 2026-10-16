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
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace xone {
// Bounded queues, independent anomaly capacity, no filesystem IO on producers.
// The detached slow-writer fallback owns shared state, never a destroyed object.
class DiagnosticJournal {
public:
    struct Limits {size_t capacity=2048,max_bytes=8*1024*1024;unsigned backups=3;std::chrono::milliseconds shutdown{1000};};
    using Sink=std::function<void(bool,const Json &)>;
private:
    struct State {
        std::mutex mutex;std::condition_variable wake,finished;
        std::deque<Json> normal,anomalies;bool stopping=false,done=false;
        uint64_t sequence=0,dropped=0,anomaly_dropped=0,io_errors=0;
        Limits limits;std::filesystem::path directory;std::string role,session;Sink sink;
    };
    std::shared_ptr<State> state_;std::thread thread_;
    static Json row(const std::string &role,const char *operation,Json fields){
        SYSTEMTIME t{};GetSystemTime(&t);char utc[40]{};
        std::snprintf(utc,sizeof(utc),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
        return {{"operation",operation},{"utc",utc},{"monotonic_us",std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()},
                {"pid",GetCurrentProcessId()},{"thread",GetCurrentThreadId()},{"role",role},{"fields",std::move(fields)}};
    }
    static void append(State &s,bool anomaly,const Json &value){
        if(s.sink){s.sink(anomaly,value);return;}
        std::filesystem::create_directories(s.directory);
        auto path=s.directory/((anomaly?"anomalies-":"cpp-")+s.role+"-"+std::to_string(GetCurrentProcessId())+".jsonl");
        const auto text=value.dump(-1,' ',false,Json::error_handler_t::replace)+"\n";
        if(std::filesystem::exists(path) && std::filesystem::file_size(path)+text.size()>s.limits.max_bytes){
            for(unsigned n=s.limits.backups;n>0;--n){
                auto dest=path;dest+=L"."+std::to_wstring(n);std::filesystem::remove(dest);
                auto source=path;if(n>1)source+=L"."+std::to_wstring(n-1);
                if(std::filesystem::exists(source))std::filesystem::rename(source,dest);
            }
        }
        std::ofstream out(path,std::ios::binary|std::ios::app);
        if(!out || !out.write(text.data(),static_cast<std::streamsize>(text.size())))throw std::runtime_error("Diagnostic write failed");
        out.flush();if(!out)throw std::runtime_error("Diagnostic flush failed");
    }
    static void run(std::shared_ptr<State> s){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        uint64_t last_dropped=0,last_anomaly_dropped=0,last_errors=0;
        auto last_health=std::chrono::steady_clock::time_point{};
        for(;;){
            std::deque<Json> ordinary,anomalies;bool stopping;Json health;
            {
                std::unique_lock lock(s->mutex);s->wake.wait_for(lock,std::chrono::seconds(1),[&]{return s->stopping || !s->normal.empty() || !s->anomalies.empty();});
                ordinary.swap(s->normal);anomalies.swap(s->anomalies);stopping=s->stopping;
                if(s->dropped!=last_dropped || s->anomaly_dropped!=last_anomaly_dropped || s->io_errors!=last_errors){
                    auto loss=row(s->role,"trace_loss",{{"dropped",s->dropped},{"anomaly_dropped",s->anomaly_dropped},{"io_errors",s->io_errors}});
                    loss["seq"]=++s->sequence;loss["session"]=s->session;anomalies.push_back(loss);ordinary.push_back(std::move(loss));
                    last_dropped=s->dropped;last_anomaly_dropped=s->anomaly_dropped;last_errors=s->io_errors;
                }
                health={{"Dropped",s->dropped},{"AnomalyDropped",s->anomaly_dropped},{"IoErrors",s->io_errors},{"Sequence",s->sequence},{"Stopping",stopping}};
            }
            auto write=[&](bool anomaly,const Json &event){try{append(*s,anomaly,event);}catch(...){std::lock_guard lock(s->mutex);++s->io_errors;}};
            for(const auto &event:anomalies)write(true,event);
            for(const auto &event:ordinary)write(false,event);
            const auto now=std::chrono::steady_clock::now();
            if(!s->sink && (stopping || now-last_health>=std::chrono::seconds(1)))try{
                last_health=now;
                {std::lock_guard lock(s->mutex);health["IoErrors"]=s->io_errors;}
                const auto path=s->directory/(s->role+"-"+std::to_string(GetCurrentProcessId())+"-health.json");
                auto temporary=path;temporary+=L".tmp";
                {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);const auto text=health.dump();if(!out || !out.write(text.data(),static_cast<std::streamsize>(text.size())))throw std::runtime_error("Diagnostic health write failed");out.flush();if(!out)throw std::runtime_error("Diagnostic health flush failed");}
                if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("Diagnostic health replace failed");
            }catch(...){std::lock_guard lock(s->mutex);++s->io_errors;}
            if(stopping)break;
        }
        {std::lock_guard lock(s->mutex);s->done=true;}s->finished.notify_all();
    }
public:
    DiagnosticJournal(std::filesystem::path directory,std::string role,Limits limits,Sink sink={}):state_(std::make_shared<State>()){
        if(!limits.capacity || !limits.max_bytes || !limits.backups)throw std::invalid_argument("Invalid journal limits");
        state_->directory=std::move(directory);state_->role=std::move(role);state_->limits=limits;state_->sink=std::move(sink);
        const auto session=state_->directory.filename().u8string();state_->session=std::string(session.begin(),session.end());
        thread_=std::thread(run,state_);
    }
    ~DiagnosticJournal(){
        event("journal_stopping");
        {std::lock_guard lock(state_->mutex);state_->stopping=true;}state_->wake.notify_one();
        std::unique_lock lock(state_->mutex);const bool done=state_->finished.wait_for(lock,state_->limits.shutdown,[&]{return state_->done;});lock.unlock();
        if(done)thread_.join();else thread_.detach();
    }
    void event(const char *operation,Json fields={},bool anomaly=false)noexcept{
        try{
            auto record=row(state_->role,operation,std::move(fields));record["session"]=state_->session;
            std::lock_guard lock(state_->mutex);if(state_->stopping)return;record["seq"]=++state_->sequence;
            if(anomaly){if(state_->anomalies.size()<state_->limits.capacity)state_->anomalies.push_back(record);else ++state_->anomaly_dropped;}
            if(state_->normal.size()<state_->limits.capacity)state_->normal.push_back(std::move(record));else ++state_->dropped;
            state_->wake.notify_one();
        }catch(...){}
    }
};
inline std::unique_ptr<DiagnosticJournal> diagnostic_journal;
inline bool diagnostic_verbose=false;
inline void initialize_diagnostics(const std::string &role,const std::filesystem::path &directory={})noexcept{
    try{
        if(diagnostic_journal)return;
        wchar_t configured[32768]{};const auto size=GetEnvironmentVariableW(L"XONE_CPP_LOG_DIR",configured,32768);
        auto path=directory;
        if(size && size<32768)path=configured;
        if(path.empty()){
            wchar_t exe[32768]{};const auto n=GetModuleFileNameW(nullptr,exe,32768);if(!n || n>=32768)return;
            auto root=std::filesystem::path(exe).parent_path().parent_path();if(root.filename()==L"outputs")root=root.parent_path();path=root/L"work/diagnostics/cpp";
        }
        wchar_t verbose[4]{};diagnostic_verbose=GetEnvironmentVariableW(L"XONE_CPP_TRACE",verbose,4)==1 && verbose[0]==L'1';
        diagnostic_journal=std::make_unique<DiagnosticJournal>(path,role,DiagnosticJournal::Limits{});
        diagnostic_journal->event("process_started",{{"verbose",diagnostic_verbose}});
    }catch(...){}
}
inline void diagnostic(const char *operation,Json fields={},bool anomaly=false)noexcept{
    if(diagnostic_journal)diagnostic_journal->event(operation,std::move(fields),anomaly);
}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "audio_output.hpp"
#include "audio_ipc.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include "audio_scheduling.hpp"
namespace xone {
using AudioBytes=std::vector<uint8_t>;
namespace {
std::atomic_ulong audio_pipe_id=0;
std::wstring host_path(){
    wchar_t path[4096]{};if(!GetModuleFileNameW(nullptr,path,4096))throw std::runtime_error("Cannot locate native audio host");
    std::wstring name=path;const auto at=name.find_last_of(L"\\/");if(at==name.npos)throw std::runtime_error("Invalid audio host path");
    return name.substr(0,at+1)+L"native-audio-probe.exe";
}
bool read(HANDLE pipe,AudioMessage &message){
    DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))throw std::runtime_error("Native audio host pipe disconnected");
    if(!available)return false;DWORD done=0;
    if(!ReadFile(pipe,message.data(),static_cast<DWORD>(message.size()),&done,nullptr) || done!=message.size())throw std::runtime_error("Invalid native audio host message");
    audio_payload(message);return true;
}
bool write(HANDLE pipe,const AudioMessage &message){
    DWORD done=0;if(!WriteFile(pipe,message.data(),static_cast<DWORD>(message.size()),&done,nullptr))return false;
    if(done==message.size())return true;
    // A full message pipe in PIPE_NOWAIT mode can succeed without writing bytes.
    SetLastError(done==0?ERROR_NO_DATA:ERROR_BAD_LENGTH);return false;
}
}
struct NativeAudioOutput::Impl {
    struct Target {
        HANDLE pipe=INVALID_HANDLE_VALUE,process=nullptr;bool attached=false;uint8_t client;
        std::deque<AudioBytes> samples;AudioEndpoints endpoints;
        std::deque<AudioBytes> microphone;
        std::mutex mutex;std::exception_ptr error;std::atomic_bool stopped=false;
        std::thread worker;
        explicit Target(uint8_t id):client(id){}
        ~Target(){
            stopped=true;if(worker.joinable())worker.join();
            if(pipe!=INVALID_HANDLE_VALUE){if(attached)write(pipe,audio_message(4));CloseHandle(pipe);}
            if(process){if(WaitForSingleObject(process,3000)==WAIT_TIMEOUT){TerminateProcess(process,1);WaitForSingleObject(process,1000);}CloseHandle(process);}
        }
        void check(){if(process && WaitForSingleObject(process,0)==WAIT_OBJECT_0){DWORD code=0;GetExitCodeProcess(process,&code);throw std::runtime_error("Native audio host exited (code "+std::to_string(code)+"); inspect its private crash trace");}}
        void receive(){
            check();for(unsigned n=0;n<32;++n){AudioMessage m{};if(!read(pipe,m))break;const auto p=audio_payload(m);
                if(m[0]==2){
                    if(p.size()!=1536)throw std::runtime_error("Unexpected native audio playback format");
                    std::lock_guard lock(mutex);if(samples.size()>=4)samples.pop_front();samples.emplace_back(p.begin(),p.end());
                }else if(m[0]==5){
                    if(p.empty() || p.size()>256 || std::find(p.begin(),p.end(),0)!=p.end())throw std::runtime_error("Invalid audio endpoint ID");
                    std::lock_guard lock(mutex);auto &id=m[1]?endpoints.capture:endpoints.render;id.assign(reinterpret_cast<const char *>(p.data()),p.size());
                    if(id.find_first_not_of("{}.0123456789abcdefABCDEF-")!=std::string::npos)throw std::runtime_error("Unexpected audio endpoint ID characters");
                    std::cout<<"NATIVE AUDIO "<<(m[1]?"CAPTURE":"RENDER")<<" ID="<<id<<'\n';
                }else throw std::runtime_error("Unexpected audio host response");
            }
        }
        void start(){
            const auto name=L"\\\\.\\pipe\\xone-audio-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++audio_pipe_id);
            pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr);
            if(pipe==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create native audio pipe");
            const auto executable=host_path();std::wstring command=L"\""+executable+L"\" --host "+name;
            STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION info{};if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))throw std::runtime_error("Cannot start native audio host");
            process=info.hProcess;CloseHandle(info.hThread);
            bool connected=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            while(std::chrono::steady_clock::now()<deadline){
                if(stopped)return;
                check();if(!connected && (ConnectNamedPipe(pipe,nullptr) || GetLastError()==ERROR_PIPE_CONNECTED))connected=true;
                if(connected){AudioMessage ready{};if(read(pipe,ready)){if(ready[0]!=3 || !audio_payload(ready).empty())throw std::runtime_error("Invalid native audio handshake");attached=true;return;}}
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            throw std::runtime_error("Native audio host startup timed out");
        }
        void launch(){worker=std::thread([this]{
            try{
                // Host startup and pipe IO must not block another peer's radio/audio.
                start();xone::AudioScheduling scheduling;xone::AudioTick tick;
                while(!stopped){
                    receive();std::deque<AudioBytes> queued;
                    {std::lock_guard lock(mutex);queued.swap(microphone);}
                    for(const auto &pcm:queued)if(!write(pipe,audio_message(1,pcm)) && GetLastError()!=ERROR_NO_DATA)throw std::runtime_error("Native audio capture pipe failed");
                    tick.wait();
                }
            }catch(...){std::lock_guard lock(mutex);error=std::current_exception();}
        });}
        void check_error(){if(error)std::rethrow_exception(error);}
    };
    std::array<std::unique_ptr<Target>,16> targets;
    std::mutex retire_mutex;std::condition_variable retire_changed;
    std::deque<std::unique_ptr<Target>> retired;bool closing=false;
    std::thread reaper;
    Impl():reaper([this]{
        for(;;){std::unique_ptr<Target> target;
            {std::unique_lock lock(retire_mutex);retire_changed.wait(lock,[&]{return closing || !retired.empty();});if(retired.empty())return;target=std::move(retired.front());retired.pop_front();}
            target.reset();
        }
    }){}
    ~Impl(){for(auto &target:targets)retire(std::move(target));{std::lock_guard lock(retire_mutex);closing=true;}retire_changed.notify_one();reaper.join();}
    void retire(std::unique_ptr<Target> target){if(!target)return;target->stopped=true;{std::lock_guard lock(retire_mutex);retired.push_back(std::move(target));}retire_changed.notify_one();}
    Target *target(uint8_t wcid,uint8_t client){if(!wcid || wcid>16)return nullptr;auto &p=targets[wcid-1];return p && p->client==client?p.get():nullptr;}
};
NativeAudioOutput::NativeAudioOutput():impl_(std::make_unique<Impl>()){}NativeAudioOutput::~NativeAudioOutput()=default;
void NativeAudioOutput::connect(uint8_t wcid,uint8_t client){
    if(!wcid || wcid>16 || client>15)throw std::runtime_error("Invalid native audio target");
    if(impl_->targets[wcid-1])return;
    auto target=std::make_unique<Impl::Target>(client);target->launch();impl_->targets[wcid-1]=std::move(target);
    std::cout<<"NATIVE AUDIO REQUESTED WCID="<<unsigned(wcid)<<" client="<<unsigned(client)<<" synthetic_parent=gamepad\n";
}
void NativeAudioOutput::disconnect(uint8_t wcid){if(wcid && wcid<=16)impl_->retire(std::move(impl_->targets[wcid-1]));}
void NativeAudioOutput::capture(uint8_t wcid,uint8_t client,std::span<const uint8_t> pcm){
    if(pcm.size()!=384)throw std::runtime_error("Unexpected native microphone format");
    if(auto *t=impl_->target(wcid,client)){std::lock_guard lock(t->mutex);t->check_error();if(t->microphone.size()>=4)t->microphone.pop_front();t->microphone.emplace_back(pcm.begin(),pcm.end());}
}
AudioBytes NativeAudioOutput::playback(uint8_t wcid,uint8_t client){
    if(auto *t=impl_->target(wcid,client)){std::lock_guard lock(t->mutex);t->check_error();if(!t->samples.empty()){auto out=std::move(t->samples.front());t->samples.pop_front();return out;}}
    return {};
}
AudioEndpoints NativeAudioOutput::endpoints(uint8_t wcid,uint8_t client){if(auto *t=impl_->target(wcid,client)){std::lock_guard lock(t->mutex);return t->endpoints;}return {};}
}

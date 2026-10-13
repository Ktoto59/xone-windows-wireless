// SPDX-License-Identifier: GPL-2.0-or-later
// Keep the undocumented XboxGipSynthetic API in a dedicated host process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "input_output.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
namespace xone {
namespace {
std::atomic_ulong pipe_id=0;
std::wstring host_path(){wchar_t name[4096]{};if(!GetModuleFileNameW(nullptr,name,4096))throw std::runtime_error("Cannot find native host");std::wstring p=name;const auto slash=p.find_last_of(L"\\/");if(slash==p.npos)throw std::runtime_error("Invalid executable path");return p.substr(0,slash+1)+L"native-gip-probe.exe";}
void write_message(HANDLE pipe,std::span<const uint8_t> message){DWORD done=0;if(!WriteFile(pipe,message.data(),static_cast<DWORD>(message.size()),&done,nullptr) || done!=message.size())throw std::runtime_error("Native GIP host pipe write failed");}
bool read_message(HANDLE pipe,std::array<uint8_t,12> &message){DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))throw std::runtime_error("Native GIP host disconnected");if(!available)return false;DWORD done=0;if(!ReadFile(pipe,message.data(),static_cast<DWORD>(message.size()),&done,nullptr) || done!=message.size())throw std::runtime_error("Invalid native GIP host response");return true;}
}
struct NativeGipOutput::Impl {
    struct Target {
        HANDLE pipe=INVALID_HANDLE_VALUE,process=nullptr;bool connected=false;uint8_t wcid;
        std::optional<std::array<uint8_t,9>> pending;
        explicit Target(uint8_t w):wcid(w){}
        ~Target(){
            if(pipe!=INVALID_HANDLE_VALUE){if(connected){std::array<uint8_t,16> stop{};stop[0]=4;DWORD done=0;WriteFile(pipe,stop.data(),static_cast<DWORD>(stop.size()),&done,nullptr);}CloseHandle(pipe);}
            if(process){if(WaitForSingleObject(process,3000)==WAIT_TIMEOUT){TerminateProcess(process,1);WaitForSingleObject(process,1000);}CloseHandle(process);}
        }
        void check(){if(process && WaitForSingleObject(process,0)==WAIT_OBJECT_0)throw std::runtime_error("Native GIP host exited; inspect its log");}
        void receive(){check();for(unsigned n=0;n<32;++n){std::array<uint8_t,12> response{};if(!read_message(pipe,response))break;if(response[0]!=2)throw std::runtime_error("Unexpected native GIP host response");if(auto packet=native_rumble_report(std::span(response).subspan(1,10)))pending=packet;}}
        void start(){
            const auto name=L"\\\\.\\pipe\\xone-gip-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++pipe_id);
            pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr);
            if(pipe==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create native GIP host pipe");
            const auto executable=host_path();std::wstring command=L"\""+executable+L"\" --host "+name;
            STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION info{};
            if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))throw std::runtime_error("Cannot launch native-gip-probe.exe host; build-native-probe.cmd first");
            process=info.hProcess;CloseHandle(info.hThread);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            bool attached=false;
            while(std::chrono::steady_clock::now()<deadline){
                check();
                if(!attached){if(ConnectNamedPipe(pipe,nullptr) || GetLastError()==ERROR_PIPE_CONNECTED)attached=true;}
                if(attached){std::array<uint8_t,12> ready{};if(read_message(pipe,ready)){if(ready[0]!=3)throw std::runtime_error("Invalid native GIP host handshake");connected=true;return;}}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            throw std::runtime_error("Native GIP host startup timed out (requires Developer Mode and administrator)");
        }
    };
    std::array<std::unique_ptr<Target>,16> targets;
};
NativeGipOutput::NativeGipOutput():impl_(std::make_unique<Impl>()){}
NativeGipOutput::~NativeGipOutput()=default;
void NativeGipOutput::connect(uint8_t wcid){
    if(!wcid || wcid>16)throw std::runtime_error("Invalid native GIP WCID");if(impl_->targets[wcid-1])return;
    auto target=std::make_unique<Impl::Target>(wcid);target->start();impl_->targets[wcid-1]=std::move(target);
    std::fprintf(stdout,"NATIVE GIP CONNECTED WCID=%u\n",unsigned(wcid));std::fflush(stdout);
}
void NativeGipOutput::update(uint8_t wcid,const GamepadInput &p,bool guide){
    if(!wcid || wcid>16 || !impl_->targets[wcid-1])return;auto &target=*impl_->targets[wcid-1];target.check();
    std::array<uint8_t,16> message{};message[0]=1;message[1]=guide;const auto input=native_input_report(p);std::copy(input.begin(),input.end(),message.begin()+2);write_message(target.pipe,message);
}
void NativeGipOutput::disconnect(uint8_t wcid){if(wcid && wcid<=16 && impl_->targets[wcid-1]){impl_->targets[wcid-1].reset();std::fprintf(stdout,"NATIVE GIP DISCONNECTED WCID=%u\n",unsigned(wcid));std::fflush(stdout);}}
std::optional<Feedback> NativeGipOutput::feedback(){for(const auto &target:impl_->targets)if(target){target->receive();if(target->pending){Feedback result{target->wcid,0,0,target->pending};target->pending.reset();return result;}}return {};}
}

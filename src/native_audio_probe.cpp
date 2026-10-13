// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated experiment with the system XboxGipSynthetic audio exports.
// Does not open USB, change default audio endpoints, or record microphone audio.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <roapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl.h>
#include <iostream>
#include <map>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <mutex>
#include <deque>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "audio_ipc.hpp"
#include "synthetic_audio.hpp"
#include "crash_trace.hpp"
#include "audio_scheduling.hpp"
namespace {
using Microsoft::WRL::ComPtr;
using Callback=void(CALLBACK *)(void *,ULONG,const void *,ULONG,void *);
void checked(HRESULT hr,const char *operation){
    std::cout<<operation<<" HRESULT=0x"<<std::hex<<static_cast<unsigned>(hr)<<std::dec<<'\n';
    if(FAILED(hr))throw std::runtime_error(operation);
}
template<class T>T resolve(HMODULE dll,const char *name){
    const auto entry=GetProcAddress(dll,name);if(!entry)throw std::runtime_error(name);
    return reinterpret_cast<T>(entry);
}
void quiet(HRESULT hr,const char *operation){if(FAILED(hr))checked(hr,operation);}
std::string utf8(std::wstring_view text){
    if(text.empty())return {};const auto length=static_cast<int>(text.size());
    const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),length,nullptr,0,nullptr,nullptr);std::string out(n,0);
    if(n)WideCharToMultiByte(CP_UTF8,0,text.data(),length,out.data(),n,nullptr,nullptr);return out;
}
struct Feedback {
    std::atomic_uint64_t count=0,bytes=0,dropped=0;std::atomic_ulong last_type=0,last_size=0;
    std::mutex mutex;std::deque<std::vector<uint8_t>> playback;
    uint64_t samples=0,clipped=0;int peak=0,max_step=0,max_inside=0,max_boundary=0;
    std::array<uint64_t,8> jump_bins{};
    std::array<int,2> previous{};bool have_previous=false;
    bool detailed=false;
    std::atomic_uint64_t invalid_context=0;
    std::atomic_bool closing=false;
};
// One host owns one device. Callback storage survives asynchronous teardown.
Feedback &host_feedback(){static auto *feedback=new Feedback;return *feedback;}
void CALLBACK callback(void *,ULONG type,const void *buffer,ULONG size,void *context)noexcept{
    auto &f=host_feedback();
    if(context!=&f){++f.invalid_context;return;}
    if(!buffer || f.closing)return;
    ++f.count;f.bytes+=size;f.last_type=type;f.last_size=size;
    if(type==1 && size==1538)try{
        std::lock_guard lock(f.mutex);if(f.playback.size()>=8){f.playback.pop_front();++f.dropped;}
        const auto *b=static_cast<const uint8_t *>(buffer);
        if(f.detailed)for(size_t at=2;at+3<size;at+=4){
            for(unsigned channel=0;channel<2;++channel){
                int16_t sample=0;std::memcpy(&sample,b+at+channel*2,2);const auto value=int(sample);
                f.peak=std::max(f.peak,std::abs(value));if(std::abs(value)>=32760)++f.clipped;
                if(f.have_previous){
                    const auto step=std::abs(value-f.previous[channel]);
                    f.max_step=std::max(f.max_step,step);
                    if(at==2)f.max_boundary=std::max(f.max_boundary,step);
                    else f.max_inside=std::max(f.max_inside,step);
                    if(step>512)++f.jump_bins[((at-2)/4)/48];
                }
                f.previous[channel]=value;++f.samples;
            }
            f.have_previous=true;
        }
        f.playback.emplace_back(b+2,b+size);
    }catch(...){}
}
struct Endpoint {EDataFlow flow;DWORD state;std::wstring name;};
using Endpoints=std::map<std::wstring,Endpoint>;
Endpoints endpoints(IMMDeviceEnumerator &enumerator){
    Endpoints out;
    for(const auto flow:{eRender,eCapture}){
        ComPtr<IMMDeviceCollection> collection;
        quiet(enumerator.EnumAudioEndpoints(flow,DEVICE_STATEMASK_ALL,&collection),"EnumAudioEndpoints");
        UINT count=0;quiet(collection->GetCount(&count),"EndpointCount");
        for(UINT n=0;n<count;++n){
            ComPtr<IMMDevice> device;quiet(collection->Item(n,&device),"EndpointItem");
            LPWSTR id=nullptr;quiet(device->GetId(&id),"EndpointId");
            std::wstring key(id);CoTaskMemFree(id);
            DWORD state=0;quiet(device->GetState(&state),"EndpointState");
            ComPtr<IPropertyStore> properties;quiet(device->OpenPropertyStore(STGM_READ,&properties),"EndpointProperties");
            PROPVARIANT value{};PropVariantInit(&value);
            const auto hr=properties->GetValue(PKEY_Device_FriendlyName,&value);
            std::wstring name;if(SUCCEEDED(hr) && value.vt==VT_LPWSTR && value.pwszVal)name=value.pwszVal;
            PropVariantClear(&value);out.emplace(std::move(key),Endpoint{flow,state,std::move(name)});
        }
    }
    return out;
}
void describe(IMMDeviceEnumerator &enumerator,const std::wstring &id,const Endpoint &endpoint){
    std::cout<<"NEW ENDPOINT flow="<<(endpoint.flow==eRender?"render":"capture")<<" state="<<endpoint.state<<" id="<<utf8(id)<<" name="<<utf8(endpoint.name)<<'\n';
    if(!(endpoint.state&DEVICE_STATE_ACTIVE))return;
    ComPtr<IMMDevice> device;checked(enumerator.GetDevice(id.c_str(),&device),"NewEndpoint");
    ComPtr<IAudioClient> client;checked(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client),"AudioClient");
    WAVEFORMATEX *format=nullptr;checked(client->GetMixFormat(&format),"MixFormat");
    std::cout<<"MIX FORMAT channels="<<format->nChannels<<" rate="<<format->nSamplesPerSec<<" bits="<<format->wBitsPerSample<<" tag="<<format->wFormatTag<<"\n";
    CoTaskMemFree(format);
}
}
int main(int argc,char **argv){
    std::cout.setf(std::ios::unitbuf);SetUnhandledExceptionFilter(xone_crash_trace);
    if(FAILED(RoInitialize(RO_INIT_MULTITHREADED)))return 1;
    struct Runtime{~Runtime(){RoUninitialize();}} runtime;
    const auto dll=LoadLibraryExW(L"xboxgipsynthetic.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll){std::cerr<<"XboxGipSynthetic unavailable\n";return 1;}
    // Let process exit unload the system DLL after threads stop. Do not unload
    // it explicitly while the API can still own asynchronous callbacks.
    try{
        ComPtr<IMMDeviceEnumerator> enumerator;
        checked(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"MMDeviceEnumerator");
        const auto before=endpoints(*enumerator.Get());
        const auto create=resolve<HRESULT(WINAPI *)(ULONG,void **)>(dll,"SyntheticController_CreateController");
        const auto connect=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_Connect");
        const auto disconnect=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_Disconnect");
        const auto remove=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_RemoveController");
        const auto add_audio=resolve<HRESULT(WINAPI *)(void *,Callback,void *)>(dll,"SyntheticController_AddAudioHeadset");
        const auto remove_audio=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_RemoveAudioHeadset");
        const auto send_audio=resolve<HRESULT(WINAPI *)(void *,const void *,ULONG)>(dll,"SyntheticController_SendAudio");
        const auto device_id=resolve<HRESULT(WINAPI *)(void *,ULONGLONG *)>(dll,"SyntheticController_GetDeviceId");
        auto &feedback=host_feedback();void *handle=nullptr;checked(create(0,&handle),"CreateAudioParent");
        if(!handle)throw std::runtime_error("Null synthetic controller");
        struct Controller{
            void *handle;decltype(disconnect) disconnect;decltype(remove) remove;decltype(remove_audio) remove_audio;bool audio=false;
            ~Controller(){host_feedback().closing=true;if(audio)std::cout<<"RemoveAudio HRESULT=0x"<<std::hex<<static_cast<unsigned>(remove_audio(handle))<<std::dec<<'\n';std::cout<<"Disconnect HRESULT=0x"<<std::hex<<static_cast<unsigned>(disconnect(handle))<<std::dec<<'\n';std::cout<<"RemoveParent HRESULT=0x"<<std::hex<<static_cast<unsigned>(remove(handle))<<std::dec<<'\n';}
        } controller{handle,disconnect,remove,remove_audio};
        checked(add_audio(handle,callback,&feedback),"AddAudioHeadset");controller.audio=true;
        if(argc==2 && std::string_view(argv[1])=="--lifecycle-check"){
            checked(connect(handle),"DiagnosticConnect");
            // Silent virtual device only: no adapter access or microphone.
            std::this_thread::sleep_for(std::chrono::seconds(3));
            feedback.closing=true;
            checked(remove_audio(handle),"DiagnosticRemoveAudio");controller.audio=false;
            std::cout<<"PASS audio API lifecycle callbacks="<<feedback.count<<" invalid_context="<<feedback.invalid_context<<'\n';return 0;
        }
        checked(connect(handle),"ConnectAudioParent");
        ULONGLONG id=0;checked(device_id(handle,&id),"SyntheticDeviceId");std::cout<<"DEVICE ID="<<id<<'\n';
        if(argc==3 && std::string_view(argv[1])=="--host"){
            const auto pipe=CreateFileA(argv[2],GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(pipe==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot connect native audio pipe");
            struct Pipe{HANDLE pipe;~Pipe(){CloseHandle(pipe);}} pipe_owner{pipe};
            DWORD mode=PIPE_READMODE_MESSAGE|PIPE_NOWAIT;if(!SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr))throw std::runtime_error("Audio pipe mode failed");
            auto reply=[&](const xone::AudioMessage &m){DWORD done=0;const auto ok=WriteFile(pipe,m.data(),static_cast<DWORD>(m.size()),&done,nullptr);if(ok)return done==m.size();if(GetLastError()==ERROR_NO_DATA)return false;throw std::runtime_error("Audio host write failed");};
            if(!reply(xone::audio_message(3)))throw std::runtime_error("Audio host ready failed");
            xone::AudioScheduling scheduling;
            xone::AudioTick tick;
            Endpoints discovered;auto inspect_at=std::chrono::steady_clock::now();bool running=true;uint64_t capture_packets=0;
            const auto microphone_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(running){
                for(unsigned n=0;n<32;++n){
                    DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)){running=false;break;}if(!available)break;
                    xone::AudioMessage m{};DWORD done=0;
                    if(!ReadFile(pipe,m.data(),static_cast<DWORD>(m.size()),&done,nullptr) || done!=m.size())throw std::runtime_error("Audio host read failed");
                    const auto p=xone::audio_payload(m);if(m[0]==4){running=false;break;}
                    if(m[0]!=1 || p.size()!=384)throw std::runtime_error("Invalid microphone IPC command");
                    for(const auto &block:xone::synthetic_microphone_blocks(p)){
                        const auto hr=send_audio(handle,block.data(),static_cast<ULONG>(block.size()));
                        if(hr==HRESULT_FROM_WIN32(ERROR_NOT_READY) && std::chrono::steady_clock::now()<microphone_deadline)continue;
                        if(FAILED(hr))checked(hr,"Host microphone");
                    }
                    ++capture_packets;
                }
                std::deque<std::vector<uint8_t>> playback;{std::lock_guard lock(feedback.mutex);playback.swap(feedback.playback);}
                if(running)for(const auto &samples:playback)if(!reply(xone::audio_message(2,samples)))++feedback.dropped;
                if(running && discovered.size()<2 && std::chrono::steady_clock::now()>=inspect_at){
                    const auto current=endpoints(*enumerator.Get());
                    for(const auto &[key,endpoint]:current){
                        const auto previous=before.find(key);
                        if(!(endpoint.state&DEVICE_STATE_ACTIVE) || (previous!=before.end() && (previous->second.state&DEVICE_STATE_ACTIVE)) || discovered.contains(key))continue;
                        describe(*enumerator.Get(),key,endpoint);const auto encoded=utf8(key);
                        if(reply(xone::audio_message(5,std::span(reinterpret_cast<const uint8_t *>(encoded.data()),encoded.size()),endpoint.flow==eCapture)))discovered.emplace(key,endpoint);
                    }
                    inspect_at=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                }
                tick.wait();
            }
            std::cout<<"AUDIO HOST microphone_packets="<<capture_packets<<" playback_callbacks="<<feedback.count<<" dropped="<<feedback.dropped<<'\n';return 0;
        }
        Endpoints discovered;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(std::chrono::steady_clock::now()<deadline){
            const auto current=endpoints(*enumerator.Get());
            for(const auto &[key,endpoint]:current){
                const auto previous=before.find(key);
                if((previous!=before.end() && (previous->second.state&DEVICE_STATE_ACTIVE)) || !(endpoint.state&DEVICE_STATE_ACTIVE))continue;
                if(!discovered.contains(key)){describe(*enumerator.Get(),key,endpoint);discovered.emplace(key,endpoint);}
            }
            bool render=false,capture=false;for(const auto &[key,endpoint]:discovered){(void)key;render|=endpoint.flow==eRender;capture|=endpoint.flow==eCapture;}
            if(render && capture)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        bool render=false,capture=false;for(const auto &[key,endpoint]:discovered){(void)key;render|=endpoint.flow==eRender;capture|=endpoint.flow==eCapture;}
        if(!render || !capture)throw std::runtime_error("Synthetic audio render/capture endpoints not both active");
        // Silent input checks export dispatch only; no physical microphone is accessed.
        const std::array<uint8_t,384> silence{};checked(send_audio(handle,silence.data(),static_cast<ULONG>(silence.size())),"SendAudioSilence");
        std::cout<<"AUDIO CALLBACKS count="<<feedback.count<<" bytes="<<feedback.bytes<<" last_type="<<feedback.last_type<<" last_size="<<feedback.last_size<<'\n';
        checked(remove_audio(handle),"RemoveAudioHeadset");controller.audio=false;
        const auto removal_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        bool removed=false;
        while(std::chrono::steady_clock::now()<removal_deadline){
            const auto current=endpoints(*enumerator.Get());removed=true;
            for(const auto &[key,endpoint]:discovered){(void)endpoint;const auto at=current.find(key);if(at!=current.end() && at->second.state&DEVICE_STATE_ACTIVE)removed=false;}
            if(removed)break;std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if(!removed)throw std::runtime_error("Synthetic audio endpoints still active after removal");
        std::cout<<"PASS native audio endpoints: render, capture, silent send, removal\n";return 0;
    }catch(const std::exception &e){std::cerr<<"Error: "<<e.what()<<'\n';return 1;}
}

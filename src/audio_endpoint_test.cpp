// SPDX-License-Identifier: GPL-2.0-or-later
// Test only the explicitly supplied endpoint; never change the system default.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmreg.h>
#include <wrl.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <stdexcept>
namespace {
void checked(HRESULT hr,const char *name){if(FAILED(hr)){std::cerr<<name<<" HRESULT=0x"<<std::hex<<static_cast<unsigned>(hr)<<std::dec<<'\n';throw std::runtime_error(name);}}
bool floating(const WAVEFORMATEX &format){
    if(format.wBitsPerSample!=32)return false;
    if(format.wFormatTag==WAVE_FORMAT_IEEE_FLOAT)return true;
    if(format.wFormatTag!=WAVE_FORMAT_EXTENSIBLE || format.cbSize<22)return false;
    constexpr GUID float_subtype{3,0,16,{128,0,0,170,0,56,155,113}};
    return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).SubFormat,float_subtype)!=0;
}
void sample(BYTE *buffer,size_t index,double value,bool is_float){
    if(is_float){const auto f=static_cast<float>(value);std::memcpy(buffer+index*4,&f,4);}
    else{const auto i=static_cast<int16_t>(value*32767);std::memcpy(buffer+index*2,&i,2);}
}
double value(const BYTE *buffer,size_t index,bool is_float){
    if(is_float){float f=0;std::memcpy(&f,buffer+index*4,4);return std::isfinite(f)?std::abs(double(f)):0;}
    int16_t i=0;std::memcpy(&i,buffer+index*2,2);return std::abs(double(i)/32768.0);
}
}
int wmain(int argc,wchar_t **argv){
    std::cout.setf(std::ios::unitbuf);
    if(argc<3 || argc>4){std::cerr<<"Usage: audio-endpoint-test render|render-quality|capture|capture-native <endpoint-id> [seconds]\n";return 1;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 1;
    struct Runtime{~Runtime(){CoUninitialize();}} runtime;
    try{
        const bool exclusive=std::wstring_view(argv[1])==L"render-exclusive-quality";
        const bool quality_render=exclusive || std::wstring_view(argv[1])==L"render-quality";
        const bool render=quality_render || std::wstring_view(argv[1])==L"render";
        const bool native_capture=std::wstring_view(argv[1])==L"capture-native";
        if(!render && !native_capture && std::wstring_view(argv[1])!=L"capture")throw std::runtime_error("Invalid audio test mode");
        const auto seconds=argc==4?std::stoul(argv[3]):render && !quality_render?3u:10u;
        if(seconds<1 || seconds>15)throw std::runtime_error("Audio test duration must be 1..15 seconds");
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        checked(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Enumerator");
        Microsoft::WRL::ComPtr<IMMDevice> device;checked(enumerator->GetDevice(argv[2],&device),"Endpoint");
        Microsoft::WRL::ComPtr<IMMEndpoint> endpoint;checked(device.As(&endpoint),"IMMEndpoint");
        EDataFlow flow=eAll;checked(endpoint->GetDataFlow(&flow),"DataFlow");if(flow!=(render?eRender:eCapture))throw std::runtime_error("Audio endpoint direction mismatch");
        Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
        checked(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,&volume),"EndpointVolume");
        BOOL muted=FALSE;float level=0;
        checked(volume->GetMute(&muted),"EndpointMute");checked(volume->GetMasterVolumeLevelScalar(&level),"EndpointLevel");
        std::cout<<"ENDPOINT muted="<<(muted?1:0)<<" level="<<level<<'\n';
        Microsoft::WRL::ComPtr<IAudioClient> client;checked(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client),"AudioClient");
        WAVEFORMATEX *format=nullptr;checked(client->GetMixFormat(&format),"MixFormat");
        struct Format{WAVEFORMATEX *p;~Format(){CoTaskMemFree(p);}} format_owner{format};
        WAVEFORMATEX microphone_format{WAVE_FORMAT_PCM,1,24000,48000,2,16,0};
        WAVEFORMATEX playback_format{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        if(native_capture)format=&microphone_format;
        if(exclusive)format=&playback_format;
        const bool is_float=floating(*format);
        if(!is_float && !(format->wFormatTag==WAVE_FORMAT_PCM && format->wBitsPerSample==16))throw std::runtime_error("Unsupported endpoint mix format");
        if(!format->nSamplesPerSec || !format->nChannels || format->nChannels>2 || (render && format->nChannels!=2) || format->nBlockAlign!=format->nChannels*(format->wBitsPerSample/8))throw std::runtime_error("Unsupported interleaved endpoint format");
        std::cout<<"FORMAT rate="<<format->nSamplesPerSec<<" channels="<<format->nChannels<<" bits="<<format->wBitsPerSample<<'\n';
        const DWORD stream_flags=native_capture?AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY:0;
        REFERENCE_TIME default_period=0,minimum_period=0;checked(client->GetDevicePeriod(&default_period,&minimum_period),"DevicePeriod");
        std::cout<<"PERIOD default_100ns="<<default_period<<" minimum_100ns="<<minimum_period<<'\n';
        checked(client->Initialize(exclusive?AUDCLNT_SHAREMODE_EXCLUSIVE:AUDCLNT_SHAREMODE_SHARED,stream_flags,1000000,exclusive?default_period:0,format,nullptr),"Initialize");
        UINT size=0;checked(client->GetBufferSize(&size),"BufferSize");
        Microsoft::WRL::ComPtr<IAudioRenderClient> writer;Microsoft::WRL::ComPtr<IAudioCaptureClient> reader;
        if(render){checked(client->GetService(IID_PPV_ARGS(&writer)),"RenderClient");BYTE *buffer=nullptr;checked(writer->GetBuffer(size,&buffer),"PrimeBuffer");checked(writer->ReleaseBuffer(size,AUDCLNT_BUFFERFLAGS_SILENT),"PrimeRelease");}
        else checked(client->GetService(IID_PPV_ARGS(&reader)),"CaptureClient");
        struct Stop{IAudioClient *p;~Stop(){p->Stop();}} stop{client.Get()};checked(client->Start(),"Start");
        Microsoft::WRL::ComPtr<IAudioClock> clock;checked(client->GetService(IID_PPV_ARGS(&clock)),"AudioClock");
        UINT64 frequency=0;checked(clock->GetFrequency(&frequency),"ClockFrequency");std::cout<<"CLOCK frequency="<<frequency<<'\n';
        const auto start=std::chrono::steady_clock::now(),end=start+std::chrono::seconds(seconds);
        auto report_at=start+std::chrono::seconds(1);uint64_t frames=render?size:0,packets=0;double peak=0;
        while(std::chrono::steady_clock::now()<end){
            if(render){
                UINT padding=0;checked(client->GetCurrentPadding(&padding),"Padding");if(padding>size)throw std::runtime_error("Invalid audio padding");
                const auto available=size-padding;
                if(available){
                    BYTE *buffer=nullptr;checked(writer->GetBuffer(available,&buffer),"RenderBuffer");
                    for(UINT n=0;n<available;++n){
                        const double time=double(frames+n)/format->nSamplesPerSec;
                        for(unsigned channel=0;channel<2;++channel){
                            const double phase=quality_render?(time<3?-1:std::fmod(time-3,3.5)):time;
                            const double begin=quality_render?(channel?1.2:0):(channel?1.7:.5),finish=begin+.7;
                            double amplitude=0;if(phase>=begin && phase<finish)amplitude=.04*std::min({1.0,(phase-begin)/.01,(finish-phase)/.01});
                            sample(buffer,size_t(n)*2+channel,amplitude*std::sin(6.283185307179586*(channel?880:660)*time),is_float);
                        }
                    }
                    checked(writer->ReleaseBuffer(available,0),"RenderRelease");frames+=available;++packets;
                }
            }else{
                UINT next=0;checked(reader->GetNextPacketSize(&next),"NextPacket");
                while(next){
                    BYTE *buffer=nullptr;UINT count=0;DWORD flags=0;checked(reader->GetBuffer(&buffer,&count,&flags,nullptr,nullptr),"CaptureBuffer");
                    if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT) && buffer)for(size_t n=0;n<size_t(count)*format->nChannels;++n)peak=std::max(peak,value(buffer,n,is_float));
                    checked(reader->ReleaseBuffer(count),"CaptureRelease");frames+=count;++packets;checked(reader->GetNextPacketSize(&next),"NextPacket");
                }
                if(std::chrono::steady_clock::now()>=report_at){std::cout<<"METER frames="<<frames<<" peak="<<peak<<'\n';report_at+=std::chrono::seconds(1);}
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(!packets)throw std::runtime_error("No audio buffers processed");
        if(!render){
            const auto expected=uint64_t(format->nSamplesPerSec)*seconds;
            std::cout<<"CAPTURE duration_seconds="<<seconds<<" expected_frames="<<expected<<" received_frames="<<frames<<'\n';
            if(frames<expected*9/10 || frames>expected*11/10)throw std::runtime_error("Capture frame count does not match elapsed duration");
        }
        UINT64 position=0,qpc=0;checked(clock->GetPosition(&position,&qpc),"ClockPosition");std::cout<<"CLOCK position="<<position<<" seconds="<<double(position)/frequency<<'\n';
        std::cout<<"PASS "<<(render?"render":"capture")<<" packets="<<packets<<" frames="<<frames<<" peak="<<peak<<'\n';return 0;
    }catch(const std::exception &e){std::cerr<<"Error: "<<e.what()<<'\n';return 1;}
}

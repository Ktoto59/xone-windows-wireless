// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental Windows export ABI; see docs/NATIVE-GIP.md.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <xinput.h>
#include <windows.gaming.input.h>
#include <windows.devices.power.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <iostream>
#include <array>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <mutex>
#include <cmath>
#include "input_output.hpp"
#include "crash_trace.hpp"
namespace {
using Callback=void(CALLBACK *)(void *,ULONG,const void *,ULONG,void *);
struct Capture{std::mutex mutex;std::array<uint8_t,64> data{};ULONG type=0,size=0;unsigned count=0;};
void CALLBACK callback(void *,ULONG type,const void *buffer,ULONG size,void *context)noexcept{
    if(!context || !buffer || size>64)return;
    try{auto &c=*static_cast<Capture *>(context);std::lock_guard lock(c.mutex);c.type=type;c.size=size;std::copy_n(static_cast<const uint8_t *>(buffer),size,c.data.begin());++c.count;}catch(...){}
}
template<class T>T resolve(HMODULE dll,const char *name){auto p=GetProcAddress(dll,name);if(!p)throw std::runtime_error(name);return reinterpret_cast<T>(p);}
void checked(HRESULT hr,const char *operation){std::cout<<operation<<" HRESULT=0x"<<std::hex<<static_cast<unsigned>(hr)<<std::dec<<"\n";if(FAILED(hr))throw std::runtime_error(operation);}
struct LiveWindow {
    HWND window=nullptr;
    LiveWindow(){
        WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"XoneMotorTest";cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
        window=CreateWindowW(cls.lpszClassName,L"xone: тест четырёх моторов — нажми на это окно",WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,CW_USEDEFAULT,580,160,nullptr,nullptr,cls.hInstance,nullptr);
        if(!window)throw std::runtime_error("Cannot create live test window");
        CreateWindowW(L"STATIC",L"Оставь это окно активным.\nЧетыре импульса: левый мотор, правый, левый триггер, правый.",WS_CHILD|WS_VISIBLE,20,20,530,60,window,nullptr,cls.hInstance,nullptr);
        ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
    }
    ~LiveWindow(){if(IsWindow(window))DestroyWindow(window);}
    void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}if(!IsWindow(window))throw std::runtime_error("Test window closed");}
    void wait_focus(){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(45);do{pump();if(GetForegroundWindow()==window)return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}while(std::chrono::steady_clock::now()<end);throw std::runtime_error("Test window needs foreground focus for WGI");}
    void wait(unsigned ms){const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);while(std::chrono::steady_clock::now()<end){pump();if(GetForegroundWindow()!=window)throw std::runtime_error("Test lost focus; motors stopped");std::this_thread::sleep_for(std::chrono::milliseconds(10));}}
};
}
int main(int argc,char **argv){
    std::cout.setf(std::ios::unitbuf);
    SetUnhandledExceptionFilter(xone_crash_trace);
    const auto init=RoInitialize(RO_INIT_MULTITHREADED);
    if(FAILED(init)){std::cerr<<"RoInitialize failed\n";return 1;}
    struct Runtime{~Runtime(){RoUninitialize();}} runtime;
    if(argc==2 && std::string(argv[1])=="--backend-smoke"){
        try{
            xone::NativeGipOutput output;
            for(unsigned cycle=0;cycle<3;++cycle){
                std::cout<<"BACKEND CYCLE "<<cycle+1<<"\n";output.connect(1);
                xone::GamepadInput input{16,1023,512,-32768,32767,static_cast<int16_t>(1234+cycle),-5678};output.update(1,input,false);
                bool passed=false;DWORD slot=0;const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(std::chrono::steady_clock::now()<end){
                    output.update(1,input,false);output.feedback();
                    for(DWORD n=0;n<4;++n){
                        XINPUT_STATE state{};if(XInputGetState(n,&state))continue;
                        const auto &p=state.Gamepad;
                        if(p.wButtons==0x1000 && p.bLeftTrigger==255 &&
                           (p.bRightTrigger==127 || p.bRightTrigger==128) &&
                           p.sThumbLX==input.left_x && p.sThumbLY==input.left_y &&
                           p.sThumbRX==input.right_x && p.sThumbRY==input.right_y){passed=true;slot=n;}
                    }
                    if(passed)break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if(!passed)throw std::runtime_error("Backend XInput mismatch");
                XINPUT_VIBRATION vibration{24000,12000};if(XInputSetState(slot,&vibration))throw std::runtime_error("Backend XInput vibration failed");
                bool feedback_passed=false;const auto feedback_end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                while(std::chrono::steady_clock::now()<feedback_end){if(auto f=output.feedback();f && f->packet && (*f->packet)[4]>0 && (*f->packet)[5]>0){feedback_passed=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
                vibration={};XInputSetState(slot,&vibration);if(!feedback_passed)throw std::runtime_error("Backend feedback pipe failed");
                std::cout<<"PASS BACKEND FEEDBACK\n";output.update(1,input,true);output.update(1,input,false);output.disconnect(1);std::cout<<"PASS BACKEND CYCLE "<<cycle+1<<"\n";
            }
            return 0;
        }catch(const std::exception &e){std::cerr<<"Error: "<<e.what()<<"\n";return 1;}
    }
    if(argc==2 && std::string(argv[1])=="--battery"){
        try{
            using namespace ABI::Windows::Gaming::Input;
            using Microsoft::WRL::ComPtr;
            ComPtr<IRawGameControllerStatics> statics;
            checked(RoGetActivationFactory(Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Gaming_Input_RawGameController).Get(),IID_PPV_ARGS(&statics)),"WGI raw factory");
            ComPtr<ABI::Windows::Foundation::Collections::IVectorView<RawGameController *>> controllers;
            unsigned count=0;
            for(unsigned n=0;n<100 && !count;++n){checked(statics->get_RawGameControllers(&controllers),"WGI raw list");checked(controllers->get_Size(&count),"WGI raw size");if(!count)std::this_thread::sleep_for(std::chrono::milliseconds(20));}
            std::cout<<"RAW CONTROLLERS="<<count<<'\n';
            for(unsigned n=0;n<count;++n){
                ComPtr<IRawGameController> controller;checked(controllers->GetAt(n,&controller),"WGI raw controller");
                USHORT vendor=0,product=0;checked(controller->get_HardwareVendorId(&vendor),"WGI vendor");checked(controller->get_HardwareProductId(&product),"WGI product");
                std::cout<<"RAW index="<<n<<" vid="<<std::hex<<vendor<<" pid="<<product<<std::dec<<'\n';
                ComPtr<IGameControllerBatteryInfo> info;
                const auto hr=controller.As(&info);
                if(FAILED(hr)){std::cout<<"BATTERY interface HRESULT=0x"<<std::hex<<static_cast<unsigned>(hr)<<std::dec<<'\n';continue;}
                ComPtr<ABI::Windows::Devices::Power::IBatteryReport> report;checked(info->TryGetBatteryReport(&report),"WGI battery report");
                if(!report){std::cout<<"BATTERY report=null\n";continue;}
                ABI::Windows::System::Power::BatteryStatus status{};checked(report->get_Status(&status),"WGI battery status");
                ComPtr<ABI::Windows::Foundation::IReference<INT32>> remaining,full;
                checked(report->get_RemainingCapacityInMilliwattHours(&remaining),"WGI battery remaining");
                checked(report->get_FullChargeCapacityInMilliwattHours(&full),"WGI battery full");
                INT32 remaining_value=0,full_value=0;
                if(remaining)checked(remaining->get_Value(&remaining_value),"WGI remaining value");
                if(full)checked(full->get_Value(&full_value),"WGI full value");
                std::cout<<"BATTERY status="<<static_cast<int>(status);
                if(remaining)std::cout<<" remaining="<<remaining_value;else std::cout<<" remaining=null";
                if(full)std::cout<<" full="<<full_value;else std::cout<<" full=null";
                std::cout<<'\n';
            }
            return count?0:2;
        }catch(const std::exception &e){std::cerr<<"Error: "<<e.what()<<"\n";return 1;}
    }
    if(argc==2 && std::string(argv[1])=="--live-rumble"){
        try{
            LiveWindow window;window.wait_focus();window.wait(700);
            using namespace ABI::Windows::Gaming::Input;
            Microsoft::WRL::ComPtr<IGamepadStatics> statics;
            checked(RoGetActivationFactory(Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Gaming_Input_Gamepad).Get(),IID_PPV_ARGS(&statics)),"WGI factory");
            Microsoft::WRL::ComPtr<IGamepad> pad;
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(std::chrono::steady_clock::now()<end){
                Microsoft::WRL::ComPtr<ABI::Windows::Foundation::Collections::IVectorView<Gamepad *>> pads;
                const auto hr=statics->get_Gamepads(&pads);if(FAILED(hr))checked(hr,"WGI list");unsigned count=0;pads->get_Size(&count);
                if(count>1)throw std::runtime_error("Live rumble test requires exactly one gamepad");
                if(count==1){checked(pads->GetAt(0,&pad),"WGI gamepad");break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if(!pad)throw std::runtime_error("No WGI gamepad");
            struct Stop{IGamepad *pad;~Stop(){pad->put_Vibration(GamepadVibration{});}} stop{pad.Get()};
            GamepadReading r{};checked(pad->GetCurrentReading(&r),"WGI reading");
            std::cout<<"LIVE WGI buttons="<<static_cast<unsigned>(r.Buttons)<<" LT="<<r.LeftTrigger<<" RT="<<r.RightTrigger<<" LX="<<r.LeftThumbstickX<<" LY="<<r.LeftThumbstickY<<"\n";
            const GamepadVibration pulses[]={{.25,0,0,0},{0,.25,0,0},{0,0,.5,0},{0,0,0,.5}};
            unsigned n=0;for(const auto &p:pulses){checked(pad->put_Vibration(p),"Live motor pulse");std::cout<<"MOTOR "<<++n<<"\n";window.wait(600);checked(pad->put_Vibration(GamepadVibration{}),"Live motor stop");window.wait(300);}
            return 0;
        }catch(const std::exception &e){std::cerr<<"Error: "<<e.what()<<"\n";return 1;}
    }
    auto dll=LoadLibraryExW(L"xboxgipsynthetic.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll){std::cerr<<"XboxGipSynthetic unavailable\n";return 1;}
    struct Library{HMODULE dll;~Library(){FreeLibrary(dll);}} library{dll};
    try{
        const auto create=resolve<HRESULT(WINAPI *)(ULONG,void **)>(dll,"SyntheticController_CreateController");
        const auto connect=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_Connect");
        const auto disconnect=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_Disconnect");
        const auto remove=resolve<HRESULT(WINAPI *)(void *)>(dll,"SyntheticController_RemoveController");
        const auto send=resolve<HRESULT(WINAPI *)(void *,ULONG,const void *,UINT)>(dll,"SyntheticController_SendReport");
        const auto reg=resolve<HRESULT(WINAPI *)(void *,ULONG,Callback,const void *)>(dll,"SyntheticController_RegisterReportCallback");
        const auto unreg=resolve<HRESULT(WINAPI *)(void *,ULONG)>(dll,"SyntheticController_UnregisterReportCallback");
        Capture capture;
        void *handle=nullptr;checked(create(0,&handle),"Create");
        if(!handle)throw std::runtime_error("Null synthetic controller");
        struct Controller{void *handle;decltype(disconnect) stop;decltype(remove) remove;~Controller(){std::cout<<"Disconnect HRESULT=0x"<<std::hex<<static_cast<unsigned>(stop(handle))<<"\n";std::cout<<"Remove HRESULT=0x"<<static_cast<unsigned>(remove(handle))<<std::dec<<"\n";}} controller{handle,disconnect,remove};
        checked(reg(handle,0,callback,&capture),"RegisterCallback");
        struct Registration{void *handle;decltype(unreg) unreg;~Registration(){unreg(handle,0);}} registration{handle,unreg};
        checked(connect(handle),"Connect");
        if(argc==3 && std::string(argv[1])=="--host"){
            auto pipe=CreateFileA(argv[2],GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(pipe==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot connect native host pipe");
            struct Pipe{HANDLE handle;~Pipe(){CloseHandle(handle);}} pipe_owner{pipe};
            DWORD mode=PIPE_READMODE_MESSAGE;if(!SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr))throw std::runtime_error("Native host pipe mode");
            auto reply=[&](const std::array<uint8_t,12> &m){DWORD done=0;return WriteFile(pipe,m.data(),static_cast<DWORD>(m.size()),&done,nullptr) && done==m.size();};
            std::array<uint8_t,12> ready{};ready[0]=3;if(!reply(ready))throw std::runtime_error("Native host ready response");
            bool running=true,guide=false;
            while(running){
                for(unsigned n=0;n<64;++n){
                    DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)){running=false;break;}if(!available)break;
                    std::array<uint8_t,16> message{};DWORD done=0;if(!ReadFile(pipe,message.data(),static_cast<DWORD>(message.size()),&done,nullptr) || done!=message.size()){running=false;break;}
                    if(message[0]==4){running=false;break;}
                    if(message[0]!=1)throw std::runtime_error("Invalid native host command");
                    const auto hr=send(handle,0,message.data()+2,14);if(FAILED(hr))checked(hr,"Host input");
                    if(guide!=(message[1]!=0)){const std::array<uint8_t,3> key{0,0,static_cast<uint8_t>(message[1]!=0)};checked(send(handle,1,key.data(),static_cast<UINT>(key.size())),"Host Guide");guide=message[1]!=0;}
                }
                std::array<uint8_t,12> feedback{};bool pending=false;
                {std::lock_guard lock(capture.mutex);if(capture.count && capture.type==0 && capture.size==10){feedback[0]=2;std::copy_n(capture.data.begin(),10,feedback.begin()+1);pending=true;}capture.count=0;}
                if(running && pending && !reply(feedback))running=false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return 0;
        }
        // Synthetic ABI has 16-bit triggers; the DLL scales them down to 10-bit GIP.
        std::array<uint8_t,14> report{16,0,255,255,0,128,0,128,255,127,210,4,210,233};
        checked(send(handle,0,report.data(),static_cast<UINT>(report.size())),"SendReport");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        using namespace ABI::Windows::Gaming::Input;
        Microsoft::WRL::ComPtr<IGamepadStatics> statics;
        checked(RoGetActivationFactory(Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Gaming_Input_Gamepad).Get(),IID_PPV_ARGS(&statics)),"WGI factory");
        std::array<bool,4> logged{};
        unsigned last_count=~0u;bool xinput_passed=false;
        while(std::chrono::steady_clock::now()<deadline){
            for(DWORD slot=0;slot<4;++slot){XINPUT_STATE state{};if(XInputGetState(slot,&state))continue;const auto &p=state.Gamepad;
                if(!logged[slot]){logged[slot]=true;std::cout<<"XInput slot="<<slot<<" buttons="<<p.wButtons<<" LT="<<unsigned(p.bLeftTrigger)<<" RT="<<unsigned(p.bRightTrigger)<<" LX="<<p.sThumbLX<<" LY="<<p.sThumbLY<<" RX="<<p.sThumbRX<<" RY="<<p.sThumbRY<<"\n";}
                if(!xinput_passed && p.wButtons==0x1000 && p.bLeftTrigger==255 && (p.bRightTrigger==127 || p.bRightTrigger==128) && p.sThumbLX==-32768 && p.sThumbLY==32767 && p.sThumbRX==1234 && p.sThumbRY==-5678){std::cout<<"PASS native GIP XInput slot="<<slot<<"\n";xinput_passed=true;}
            }
            Microsoft::WRL::ComPtr<ABI::Windows::Foundation::Collections::IVectorView<Gamepad *>> pads;
            const auto pads_hr=statics->get_Gamepads(&pads);if(FAILED(pads_hr))checked(pads_hr,"WGI gamepads");unsigned count=0;pads->get_Size(&count);
            if(count!=last_count){std::cout<<"WGI gamepad count="<<count<<"\n";last_count=count;}
            for(unsigned i=0;i<count;++i){Microsoft::WRL::ComPtr<IGamepad> pad;pads->GetAt(i,&pad);GamepadReading reading{};
                if(SUCCEEDED(pad->GetCurrentReading(&reading)) && reading.Buttons==GamepadButtons_A && reading.LeftTrigger>.99 && reading.RightTrigger>.49 && reading.RightTrigger<.51 && reading.LeftThumbstickX<-.99 && reading.LeftThumbstickY>.99){
                    std::cout<<"PASS native GIP WGI input (gamepad index="<<i<<")\n";
                    checked(pad->put_Vibration(GamepadVibration{.25,.5,.75,1.}),"WGI four motors");std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    {std::lock_guard lock(capture.mutex);std::cout<<"CALLBACK count="<<capture.count<<" type="<<capture.type<<" size="<<capture.size<<" hex=";for(unsigned n=0;n<capture.size;++n)std::cout<<std::hex<<unsigned(capture.data[n])<<",";std::cout<<std::dec<<"\n";}
                    checked(pad->put_Vibration(GamepadVibration{}),"WGI stop motors");if(xinput_passed)return 0;
                }
            }
            report[0]=report[0]==16?32:16;
            const auto hr=send(handle,0,report.data(),static_cast<UINT>(report.size()));if(FAILED(hr))checked(hr,"SendReport");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("Native GIP XInput state not observed");
    }catch(const std::exception &e){std::cerr<<"Error: "<<e.what()<<"\n";return 1;}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "winusb_transport.hpp"
#include "adapter_catalog.hpp"
#include "diagnostic_log.hpp"
#include <setupapi.h>
#include <devpkey.h>
#include <usbiodef.h>
#include <iostream>
#include <stdexcept>
#include <array>
#include <cwctype>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace xone {
std::string utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}
std::string win_error(DWORD error) {
    wchar_t *message = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,error,0,reinterpret_cast<wchar_t *>(&message),0,nullptr);
    std::string result = count ? utf8(std::wstring(message,count)) : "Unknown Windows error";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back()=='\r' || result.back()=='\n')) result.pop_back();
    return "Windows error " + std::to_string(error) + ": " + result;
}
static std::string transfer_error(const char *operation,uint8_t pipe,size_t requested,DWORD error) {
    diagnostic("usb_transfer_error",{{"operation",operation},{"endpoint",pipe},{"requested",requested},{"win32_error",error}},true);
    SYSTEMTIME now{};GetSystemTime(&now);
    std::ostringstream message;
    message<<operation<<": "<<win_error(error)<<" [endpoint=0x"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(pipe)<<std::dec
           <<" requested="<<requested<<" utc="<<std::setw(4)<<now.wYear<<'-'<<std::setw(2)<<now.wMonth<<'-'<<std::setw(2)<<now.wDay
           <<'T'<<std::setw(2)<<now.wHour<<':'<<std::setw(2)<<now.wMinute<<':'<<std::setw(2)<<now.wSecond<<'.'<<std::setw(3)<<now.wMilliseconds<<"Z]";
    return message.str();
}
struct InfoSet {
    HDEVINFO value;
    ~InfoSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};
static std::wstring registry_property(HDEVINFO set, SP_DEVINFO_DATA &device, DWORD key) {
    DWORD size = 0, type = 0;
    SetupDiGetDeviceRegistryPropertyW(set,&device,key,&type,nullptr,0,&size);
    if (!size) return {};
    std::vector<wchar_t> buffer((size+sizeof(wchar_t)-1)/sizeof(wchar_t)+1,0);
    if (!SetupDiGetDeviceRegistryPropertyW(set,&device,key,&type,reinterpret_cast<BYTE *>(buffer.data()),size,nullptr)) return {};
    return buffer.data();
}
static std::wstring device_property(HDEVINFO set, SP_DEVINFO_DATA &device, const DEVPROPKEY &key) {
    DWORD size = 0;
    DEVPROPTYPE type = 0;
    SetupDiGetDevicePropertyW(set,&device,&key,&type,nullptr,0,&size,0);
    if (!size) return {};
    std::vector<wchar_t> buffer((size+sizeof(wchar_t)-1)/sizeof(wchar_t)+1,0);
    if (!SetupDiGetDevicePropertyW(set,&device,&key,&type,reinterpret_cast<BYTE *>(buffer.data()),size,nullptr,0)) return {};
    return type == DEVPROP_TYPE_STRING ? buffer.data() : std::wstring{};
}
static std::wstring instance_id(HDEVINFO set, SP_DEVINFO_DATA &device) {
    std::array<wchar_t,1024> value{};
    if (!SetupDiGetDeviceInstanceIdW(set,&device,value.data(),static_cast<DWORD>(value.size()),nullptr)) return {};
    return value.data();
}
std::vector<DeviceInfo> adapters() {
    InfoSet all{SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_PRESENT|DIGCF_ALLCLASSES)};
    if (all.value == INVALID_HANDLE_VALUE) throw std::runtime_error(win_error(GetLastError()));
    std::vector<DeviceInfo> result;
    for (DWORD i = 0;;++i) {
        SP_DEVINFO_DATA device{}; device.cbSize=sizeof(device);
        if (!SetupDiEnumDeviceInfo(all.value,i,&device)) {
            if (GetLastError()!=ERROR_NO_MORE_ITEMS) throw std::runtime_error(win_error(GetLastError()));
            break;
        }
        auto id = instance_id(all.value,device);
        if (!supported_adapter_instance(id)) continue;
        auto name = registry_property(all.value,device,SPDRP_FRIENDLYNAME);
        if (name.empty()) name=registry_property(all.value,device,SPDRP_DEVICEDESC);
        result.push_back({id,name,registry_property(all.value,device,SPDRP_SERVICE),
            device_property(all.value,device,DEVPKEY_Device_DriverInfPath),
            device_property(all.value,device,DEVPKEY_Device_DriverVersion),{}});
    }
    // Enumerate only interfaces that belong to the actual adapter instance.
    for (const auto &guid : {interface_guid,GUID_DEVINTERFACE_USB_DEVICE}) {
        InfoSet interfaces{SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
        if (interfaces.value==INVALID_HANDLE_VALUE) throw std::runtime_error(win_error(GetLastError()));
        for (DWORD i=0;;++i) {
            SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(interfaces.value,nullptr,&guid,i,&iface)) {
                if (GetLastError()!=ERROR_NO_MORE_ITEMS) throw std::runtime_error(win_error(GetLastError()));
                break;
            }
            DWORD size=0;
            SetupDiGetDeviceInterfaceDetailW(interfaces.value,&iface,nullptr,0,&size,nullptr);
            if (size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            std::vector<BYTE> buffer(size);
            auto *detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
            detail->cbSize=sizeof(*detail);
            SP_DEVINFO_DATA device{}; device.cbSize=sizeof(device);
            if (!SetupDiGetDeviceInterfaceDetailW(interfaces.value,&iface,detail,size,nullptr,&device)) continue;
            const auto id=instance_id(interfaces.value,device);
            for (auto &entry : result) if (entry.instance==id) {
                const std::wstring path=detail->DevicePath;
                if (std::find(entry.paths.begin(),entry.paths.end(),path)==entry.paths.end()) entry.paths.push_back(path);
            }
        }
    }
    return result;
}
WinUsbTransport::WinUsbTransport(const DeviceInfo &device) {
    if (_wcsicmp(device.service.c_str(),L"WinUSB"))
        throw std::runtime_error("Raw USB access is unavailable: adapter is bound to " + utf8(device.service) +
            ". The experimental WinUSB package has not been installed. See docs/WINUSB.md. No driver was changed.");
    DWORD last=ERROR_FILE_NOT_FOUND;
    for (const auto &path : device.paths) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,nullptr);
        if (file==INVALID_HANDLE_VALUE) { last=GetLastError(); continue; }
        WINUSB_INTERFACE_HANDLE usb=nullptr;
        if (WinUsb_Initialize(file,&usb)) { file_=file; usb_=usb; break; }
        last=GetLastError(); CloseHandle(file);
    }
    if (!usb_) throw std::runtime_error("Cannot open adapter: " + win_error(last));
    try {
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (!WinUsb_QueryInterfaceSettings(usb_,0,&descriptor)) throw std::runtime_error(win_error(GetLastError()));
        bool out=false,cmd=false,wlan=false;
        for (UCHAR i=0;i<descriptor.bNumEndpoints;++i) {
            WINUSB_PIPE_INFORMATION pipe{};
            if (!WinUsb_QueryPipe(usb_,0,i,&pipe)) throw std::runtime_error(win_error(GetLastError()));
            if (pipe.PipeType != UsbdPipeTypeBulk) continue;
            out |= pipe.PipeId==0x04; cmd |= pipe.PipeId==0x85; wlan |= pipe.PipeId==0x84;
            // Keep one IN request pending instead of waking USB with a new
            // read every 250ms. Cancellation is explicit during receiver stop.
            if (!set_timeout(pipe.PipeId,(pipe.PipeId&0x80)?0:250))
                throw std::runtime_error(win_error(GetLastError()));
        }
        if (!out || !cmd || !wlan) throw std::runtime_error("Unexpected USB layout: required bulk endpoints 04/84/85 missing.");
        bridge={this,control_callback,bulk_callback,0};
    } catch (...) {
        WinUsb_Free(usb_); CloseHandle(file_); usb_=nullptr; file_=INVALID_HANDLE_VALUE; throw;
    }
}
WinUsbTransport::~WinUsbTransport() { if (usb_) WinUsb_Free(usb_); if (file_!=INVALID_HANDLE_VALUE) CloseHandle(file_); if(power_devices_!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(power_devices_); }
bool WinUsbTransport::set_timeout(uint8_t pipe,unsigned timeout) {
    return timeouts_.set(pipe,timeout,[this](uint8_t endpoint,unsigned requested){
        ULONG policy=requested;
        return WinUsb_SetPipePolicy(usb_,endpoint,PIPE_TRANSFER_TIMEOUT,sizeof(policy),&policy)!=FALSE;
    });
}
int WinUsbTransport::control_callback(void *context,u8 request,u8 type,u16 value,u16 index,void *data,u16 size,unsigned timeout) {
    const auto started=std::chrono::steady_clock::now();
    auto &self=*static_cast<WinUsbTransport *>(context);
    if (!self.set_timeout(0,timeout)){const auto error=GetLastError();std::cerr<<transfer_error("Control timeout policy",0,size,error)<<'\n';return -EREMOTEIO;}
    WINUSB_SETUP_PACKET setup{type,request,value,index,size};
    ULONG transferred=0;
    if (!WinUsb_ControlTransfer(self.usb_,setup,static_cast<PUCHAR>(data),size,&transferred,nullptr)) {
        const DWORD error=GetLastError();
        std::cerr << transfer_error("Control transfer",0,size,error) << '\n'; return -EREMOTEIO;
    }
    if(diagnostic_verbose)diagnostic("usb_control",{{"request",request},{"type",type},{"requested",size},{"actual",transferred},{"elapsed_us",std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()}});
    return static_cast<int>(transferred);
}
int WinUsbTransport::bulk_callback(void *context,u8 pipe,void *data,int size,int *actual,unsigned timeout) {
    const auto started=std::chrono::steady_clock::now();
    auto &self=*static_cast<WinUsbTransport *>(context);
    ULONG transferred=0;
    if (!self.set_timeout(pipe,timeout)){const auto error=GetLastError();std::cerr<<transfer_error("Write timeout policy",pipe,static_cast<size_t>(size),error)<<'\n';return -EREMOTEIO;}
    if (!WinUsb_WritePipe(self.usb_,pipe,static_cast<PUCHAR>(data),static_cast<ULONG>(size),&transferred,nullptr)) {
        const DWORD error=GetLastError();
        std::cerr << transfer_error("Bulk write",pipe,static_cast<size_t>(size),error) << '\n'; return -EREMOTEIO;
    }
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
    if(diagnostic_verbose || elapsed>8000 || transferred!=static_cast<ULONG>(size))diagnostic("usb_write",{{"endpoint",pipe},{"requested",size},{"actual",transferred},{"elapsed_us",elapsed}},elapsed>8000 || transferred!=static_cast<ULONG>(size));
    *actual=static_cast<int>(transferred); return 0;
}
bool WinUsbTransport::prepare_idle_power(const DeviceInfo &device){
    USB_CONFIGURATION_DESCRIPTOR config{};ULONG actual=0;
    if(!WinUsb_GetDescriptor(usb_,USB_CONFIGURATION_DESCRIPTOR_TYPE,0,0,reinterpret_cast<PUCHAR>(&config),sizeof(config),&actual))
        throw std::runtime_error(transfer_error("Configuration descriptor",0,sizeof(config),GetLastError()));
    if(actual!=sizeof(config) || !(config.bmAttributes&0x20)){
        diagnostic("idle_power_unavailable",{{"reason","USB remote wake is not advertised"}},true);return false;
    }
    InfoSet devices{SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_PRESENT|DIGCF_ALLCLASSES)};
    SP_DEVINFO_DATA info{};info.cbSize=sizeof(info);
    if(devices.value==INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(devices.value,device.instance.c_str(),nullptr,0,&info))
        throw std::runtime_error("Cannot inspect adapter idle-power configuration: "+win_error(GetLastError()));
    const HKEY key=SetupDiOpenDevRegKey(devices.value,&info,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE);
    const auto key_error=key==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
    DWORD enabled=0,ignore_wake=0,size=sizeof(DWORD);
    if(key!=INVALID_HANDLE_VALUE){
        if(RegGetValueW(key,nullptr,L"DeviceIdleEnabled",RRF_RT_REG_DWORD,nullptr,&enabled,&size)!=ERROR_SUCCESS)enabled=0;
        size=sizeof(DWORD);RegGetValueW(key,nullptr,L"DeviceIdleIgnoreWakeEnable",RRF_RT_REG_DWORD,nullptr,&ignore_wake,&size);
        RegCloseKey(key);
    }
    if(!enabled || ignore_wake){
        diagnostic("idle_power_unavailable",{{"reason","Adapter requires wake-capable idle configuration; run Set-AdapterIdlePower.ps1"},{"key_error",key_error},{"idle_enabled",enabled},{"ignore_wake",ignore_wake}},true);return false;
    }
    UCHAR suspend=FALSE;
    if(!WinUsb_SetPowerPolicy(usb_,AUTO_SUSPEND,sizeof(suspend),&suspend))throw std::runtime_error("Disable USB autosuspend: "+win_error(GetLastError()));
    ULONG delay=1000;
    if(!WinUsb_SetPowerPolicy(usb_,SUSPEND_DELAY,sizeof(delay),&delay))throw std::runtime_error("USB suspend delay: "+win_error(GetLastError()));
    idle_power_ready_=true;
    power_devices_=devices.value;devices.value=INVALID_HANDLE_VALUE;power_info_=info;
    diagnostic("idle_power_ready",{{"usb_suspend_delay_ms",delay},{"remote_wake",true}});
    return true;
}
void WinUsbTransport::auto_suspend(bool enabled){
    if(!idle_power_ready_)throw std::runtime_error("USB idle power is not prepared");
    UCHAR value=enabled?TRUE:FALSE;
    if(!WinUsb_SetPowerPolicy(usb_,AUTO_SUSPEND,sizeof(value),&value)){
        const auto error=GetLastError();diagnostic("usb_power_policy_failed",{{"enable",enabled},{"win32_error",error}},true);
        throw std::runtime_error("USB autosuspend: "+win_error(error));
    }
    idle_seen_low_power_=false;
}
bool WinUsbTransport::idle_wake_observed(){
    if(!idle_power_ready_ || power_observation_failed_)return false;
    CM_POWER_DATA power{};DWORD type=0,actual=0;
    // This is a cached PnP property, not an adapter control transfer. Reading it
    // must not wake USB. Firmware may signal USB remote wake before delivering
    // any packet; normal MAC routing must then be restored to receive association.
    if(!SetupDiGetDeviceRegistryPropertyW(power_devices_,&power_info_,SPDRP_DEVICE_POWER_DATA,&type,reinterpret_cast<PBYTE>(&power),sizeof(power),&actual) || actual<sizeof(power)){
        const auto error=GetLastError();power_observation_failed_=true;
        diagnostic("usb_power_observation_failed",{{"win32_error",error}},true);return false;
    }
    if(power.PD_MostRecentPowerState==PowerDeviceD1 || power.PD_MostRecentPowerState==PowerDeviceD2 || power.PD_MostRecentPowerState==PowerDeviceD3)idle_seen_low_power_=true;
    return idle_seen_low_power_ && power.PD_MostRecentPowerState==PowerDeviceD0;
}
std::vector<uint8_t> WinUsbTransport::read(uint8_t pipe,std::stop_token stop) {
    const auto started=std::chrono::steady_clock::now();
    std::vector<uint8_t> data(0x8400);
    ULONG transferred=0;
    struct Event{HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}} event;
    if(!event.value)throw std::runtime_error(win_error(GetLastError()));
    OVERLAPPED operation{};operation.hEvent=event.value;
    if (!WinUsb_ReadPipe(usb_,pipe,data.data(),static_cast<ULONG>(data.size()),&transferred,&operation)) {
        const auto error=GetLastError();
        if(error!=ERROR_IO_PENDING)throw std::runtime_error(transfer_error("Bulk read",pipe,data.size(),error));
        DWORD wait_error=ERROR_SUCCESS;
        while(!stop.stop_requested()){
            const auto result=WaitForSingleObject(event.value,100);
            if(result==WAIT_OBJECT_0)break;
            if(result==WAIT_FAILED){wait_error=GetLastError();break;}
        }
        if(stop.stop_requested() || wait_error)CancelIoEx(file_,&operation);
        // Drain completion before destroying OVERLAPPED, event, or data buffer.
        const BOOL completed=WinUsb_GetOverlappedResult(usb_,&operation,&transferred,TRUE);
        const auto completion_error=completed?ERROR_SUCCESS:GetLastError();
        if(wait_error)throw std::runtime_error(win_error(wait_error));
        if(!completed){
            if(completion_error==ERROR_OPERATION_ABORTED && stop.stop_requested())return {};
            throw std::runtime_error(transfer_error("Bulk read",pipe,data.size(),completion_error));
        }
    }
    if(diagnostic_verbose)diagnostic("usb_read",{{"endpoint",pipe},{"actual",transferred},{"elapsed_us",std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()}});
    data.resize(transferred); return data;
}
void WinUsbTransport::describe() const {
    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (!WinUsb_QueryInterfaceSettings(usb_,0,&descriptor)) throw std::runtime_error(win_error(GetLastError()));
    std::cout << "USB interface " << unsigned(descriptor.bInterfaceNumber) << '\n';
    for (UCHAR i=0;i<descriptor.bNumEndpoints;++i) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usb_,0,i,&pipe)) throw std::runtime_error(win_error(GetLastError()));
        std::cout << "  endpoint 0x" << std::hex << unsigned(pipe.PipeId) << std::dec
            << " type=" << pipe.PipeType << " max-packet=" << pipe.MaximumPacketSize << '\n';
    }
}
}

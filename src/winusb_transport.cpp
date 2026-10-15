// SPDX-License-Identifier: GPL-2.0-or-later
#include "winusb_transport.hpp"
#include "adapter_catalog.hpp"
#include <setupapi.h>
#include <devpkey.h>
#include <usbiodef.h>
#include <iostream>
#include <stdexcept>
#include <array>
#include <cwctype>
#include <algorithm>

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
            ULONG timeout=250;
            if (!WinUsb_SetPipePolicy(usb_,pipe.PipeId,PIPE_TRANSFER_TIMEOUT,sizeof(timeout),&timeout))
                throw std::runtime_error(win_error(GetLastError()));
        }
        if (!out || !cmd || !wlan) throw std::runtime_error("Unexpected USB layout: required bulk endpoints 04/84/85 missing.");
        bridge={this,control_callback,bulk_callback,0};
    } catch (...) {
        WinUsb_Free(usb_); CloseHandle(file_); usb_=nullptr; file_=INVALID_HANDLE_VALUE; throw;
    }
}
WinUsbTransport::~WinUsbTransport() { if (usb_) WinUsb_Free(usb_); if (file_!=INVALID_HANDLE_VALUE) CloseHandle(file_); }
int WinUsbTransport::control_callback(void *context,u8 request,u8 type,u16 value,u16 index,void *data,u16 size,unsigned timeout) {
    auto &self=*static_cast<WinUsbTransport *>(context);
    ULONG policy=timeout;
    if (!WinUsb_SetPipePolicy(self.usb_,0,PIPE_TRANSFER_TIMEOUT,sizeof(policy),&policy)) return -EREMOTEIO;
    WINUSB_SETUP_PACKET setup{type,request,value,index,size};
    ULONG transferred=0;
    if (!WinUsb_ControlTransfer(self.usb_,setup,static_cast<PUCHAR>(data),size,&transferred,nullptr)) {
        std::cerr << "Control transfer: " << win_error(GetLastError()) << '\n'; return -EREMOTEIO;
    }
    return static_cast<int>(transferred);
}
int WinUsbTransport::bulk_callback(void *context,u8 pipe,void *data,int size,int *actual,unsigned timeout) {
    auto &self=*static_cast<WinUsbTransport *>(context);
    ULONG policy=timeout, transferred=0;
    if (!WinUsb_SetPipePolicy(self.usb_,pipe,PIPE_TRANSFER_TIMEOUT,sizeof(policy),&policy)) return -EREMOTEIO;
    if (!WinUsb_WritePipe(self.usb_,pipe,static_cast<PUCHAR>(data),static_cast<ULONG>(size),&transferred,nullptr)) {
        std::cerr << "Bulk write: " << win_error(GetLastError()) << '\n'; return -EREMOTEIO;
    }
    *actual=static_cast<int>(transferred); return 0;
}
std::vector<uint8_t> WinUsbTransport::read(uint8_t pipe) {
    std::vector<uint8_t> data(0x8400);
    ULONG transferred=0;
    if (!WinUsb_ReadPipe(usb_,pipe,data.data(),static_cast<ULONG>(data.size()),&transferred,nullptr)) {
        const DWORD error=GetLastError();
        if (error==ERROR_SEM_TIMEOUT) return {};
        throw std::runtime_error("Bulk read: " + win_error(error));
    }
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

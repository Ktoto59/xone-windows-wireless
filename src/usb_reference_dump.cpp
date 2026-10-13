// SPDX-License-Identifier: GPL-2.0-or-later
// Read-only hub queries for Microsoft USB descriptors. No vendor transfers,
// port resets, driver changes, microphone data, or firmware updates.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbioctl.h>
#include <usbiodef.h>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
std::string hex(const void *data,size_t size){
    const auto *p=static_cast<const unsigned char *>(data);
    constexpr char digits[]="0123456789abcdef";
    std::string out;out.reserve(size*2);
    for(size_t n=0;n<size;++n){out+=digits[p[n]>>4];out+=digits[p[n]&15];}
    return out;
}
std::vector<unsigned char> configuration(HANDLE hub,ULONG port,UCHAR index,DWORD &error){
    constexpr size_t offset=offsetof(USB_DESCRIPTOR_REQUEST,Data);
    std::vector<unsigned char> request(offset+4096);
    auto *header=reinterpret_cast<USB_DESCRIPTOR_REQUEST *>(request.data());
    header->ConnectionIndex=port;
    header->SetupPacket.wValue=static_cast<USHORT>((USB_CONFIGURATION_DESCRIPTOR_TYPE<<8)|index);
    header->SetupPacket.wLength=4096;
    DWORD returned=0;const auto size=static_cast<DWORD>(request.size());
    if(!DeviceIoControl(hub,IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,request.data(),size,request.data(),size,&returned,nullptr)){
        error=GetLastError();return {};
    }
    if(returned>request.size() || returned<offset+sizeof(USB_CONFIGURATION_DESCRIPTOR)){
        error=ERROR_INVALID_DATA;return {};
    }
    USB_CONFIGURATION_DESCRIPTOR config{};std::memcpy(&config,request.data()+offset,sizeof(config));
    if(config.bDescriptorType!=USB_CONFIGURATION_DESCRIPTOR_TYPE || config.bLength<sizeof(config) ||
       config.wTotalLength<sizeof(config) || config.wTotalLength>returned-offset){error=ERROR_INVALID_DATA;return {};}
    const auto end=offset+config.wTotalLength;
    for(size_t at=offset;at<end;){
        if(end-at<2 || request[at]<2 || request[at]>end-at){error=ERROR_INVALID_DATA;return {};}
        at+=request[at];
    }
    return {request.begin()+offset,request.begin()+end};
}
std::vector<unsigned char> os_string(HANDLE hub,ULONG port,DWORD &error){
    constexpr size_t offset=offsetof(USB_DESCRIPTOR_REQUEST,Data);
    std::vector<unsigned char> request(offset+255);
    auto *header=reinterpret_cast<USB_DESCRIPTOR_REQUEST *>(request.data());
    header->ConnectionIndex=port;
    header->SetupPacket.wValue=(USB_STRING_DESCRIPTOR_TYPE<<8)|0xee;
    header->SetupPacket.wLength=255;
    DWORD returned=0;const auto size=static_cast<DWORD>(request.size());
    if(!DeviceIoControl(hub,IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,request.data(),size,request.data(),size,&returned,nullptr)){
        error=GetLastError();return {};
    }
    if(returned>request.size() || returned<offset+2 || request[offset+1]!=USB_STRING_DESCRIPTOR_TYPE ||
       request[offset]<2 || request[offset]>returned-offset || request[offset]%2){error=ERROR_INVALID_DATA;return {};}
    return {request.begin()+offset,request.begin()+offset+request[offset]};
}
}
int main(){
    const auto devices=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(devices==INVALID_HANDLE_VALUE){std::fprintf(stderr,"Hub enumeration error=%lu\n",GetLastError());return 1;}
    struct DeviceSet {HDEVINFO value;~DeviceSet(){SetupDiDestroyDeviceInfoList(value);}} cleanup{devices};
    unsigned found=0,errors=0;
    for(DWORD n=0;;++n){
        SP_DEVICE_INTERFACE_DATA interface_data{};interface_data.cbSize=sizeof(interface_data);
        if(!SetupDiEnumDeviceInterfaces(devices,nullptr,&GUID_DEVINTERFACE_USB_HUB,n,&interface_data)){
            if(GetLastError()!=ERROR_NO_MORE_ITEMS)++errors;
            break;
        }
        DWORD needed=0;
        SetupDiGetDeviceInterfaceDetailW(devices,&interface_data,nullptr,0,&needed,nullptr);
        if(needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)){++errors;continue;}
        std::vector<unsigned char> storage(needed);
        auto *detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(devices,&interface_data,detail,needed,nullptr,nullptr)){++errors;continue;}
        Handle hub{CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
        if(hub.value==INVALID_HANDLE_VALUE){std::fprintf(stderr,"Hub %lu open error=%lu\n",n,GetLastError());++errors;continue;}
        USB_NODE_INFORMATION node{};node.NodeType=UsbHub;DWORD returned=0;
        if(!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_INFORMATION,&node,sizeof(node),&node,sizeof(node),&returned,nullptr)){
            ++errors;continue;
        }
        for(ULONG port=1;port<=node.u.HubInformation.HubDescriptor.bNumberOfPorts;++port){
            std::vector<unsigned char> buffer(sizeof(USB_NODE_CONNECTION_INFORMATION_EX)+30*sizeof(USB_PIPE_INFO));
            auto *info=reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX *>(buffer.data());info->ConnectionIndex=port;
            const auto size=static_cast<DWORD>(buffer.size());
            if(!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,info,size,info,size,&returned,nullptr)){
                ++errors;continue;
            }
            if(returned<offsetof(USB_NODE_CONNECTION_INFORMATION_EX,PipeList) ||
               info->ConnectionStatus!=DeviceConnected || info->DeviceDescriptor.idVendor!=0x045e)continue;
            ++found;const auto &d=info->DeviceDescriptor;
            std::printf("{\"HubIndex\":%lu,\"Port\":%lu,\"Vid\":%u,\"Pid\":%u,\"Revision\":%u,\"Speed\":%u,\"DeviceDescriptor\":\"%s\",\"Configurations\":[",
                n,port,unsigned(d.idVendor),unsigned(d.idProduct),unsigned(d.bcdDevice),unsigned(info->Speed),hex(&d,sizeof(d)).c_str());
            for(UCHAR index=0;index<d.bNumConfigurations;++index){
                DWORD error=0;const auto data=configuration(hub.value,port,index,error);
                if(index)std::printf(",");
                std::printf("{\"Index\":%u,\"Error\":%lu,\"Descriptor\":\"%s\"}",unsigned(index),error,hex(data.data(),data.size()).c_str());
                if(error)++errors;
            }
            std::printf("]");
            if(d.idProduct==0x0b32 || d.idProduct==0x0b17 || d.idProduct==0x0b12){
                DWORD error=0;const auto data=os_string(hub.value,port,error);
                std::printf(",\"OsString\":{\"Error\":%lu,\"Descriptor\":\"%s\"}",error,hex(data.data(),data.size()).c_str());
            }
            std::printf("}\n");
        }
    }
    std::fprintf(stderr,"Microsoft devices=%u query errors=%u\n",found,errors);
    return errors?1:0;
}

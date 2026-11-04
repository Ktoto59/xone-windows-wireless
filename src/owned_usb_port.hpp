// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
namespace xone {
enum class UsbPortOwner {absent,owned,foreign,unknown};
// usbip-win2 v.0.9.8.1 userspace/usbip/port.cpp. Empty successful output
// means no devices. Fail closed for unknown/localized/truncated identities.
inline UsbPortOwner usb_port_owner(std::string_view listing,int port,unsigned tcp_port,std::string_view serial){
    if(listing.find_first_not_of(" \r\n\t")==listing.npos)return UsbPortOwner::absent;
    if(!listing.starts_with("Imported USB devices"))return UsbPortOwner::unknown;
    std::istringstream input{std::string(listing)};std::string line;
    bool found=false,any_port=false,endpoint=false,serial_match=false;
    while(std::getline(input,line)){
        if(line.starts_with("Port ")){
            if(found)break;
            const auto colon=line.find(':',5);if(colon==line.npos)return UsbPortOwner::unknown;
            int number=0;const auto parsed=std::from_chars(line.data()+5,line.data()+colon,number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=line.data()+colon || number<1 || number>65535)return UsbPortOwner::unknown;
            any_port=true;
            found=number==port;continue;
        }
        if(!found)continue;
        const auto start=line.find_first_not_of(" \t");if(start==line.npos)continue;
        auto text=std::string_view(line).substr(start);if(!text.empty() && text.back()=='\r')text.remove_suffix(1);
        if(text.starts_with("-> usbip://")){
            const auto expected="-> usbip://127.0.0.1:"+std::to_string(tcp_port)+"/1-1";
            if(text!=expected)return UsbPortOwner::foreign;endpoint=true;
        }
        if(text.starts_with("-> serial: ")){
            if(text.substr(11)!=serial)return UsbPortOwner::foreign;serial_match=true;
        }
    }
    if(!found)return any_port?UsbPortOwner::absent:UsbPortOwner::unknown;
    return endpoint && serial_match?UsbPortOwner::owned:UsbPortOwner::unknown;
}
template<class Run,class Error>
void detach_owned_usb_port(int &port,unsigned tcp_port,std::string_view serial,Run run,Error error){
    if(!port)return;
    std::string listing;run(L"port",&listing);
    const auto owner=usb_port_owner(listing,port,tcp_port,serial);
    if(owner==UsbPortOwner::unknown)throw std::runtime_error("Cannot confirm USBip port ownership");
    if(owner==UsbPortOwner::absent || owner==UsbPortOwner::foreign){port=0;return;}
    try{run(L"detach -p "+std::to_wstring(port),nullptr);}
    catch(const std::exception &failure){
        const std::string original=failure.what();error(original);
        try{listing.clear();run(L"port",&listing);}
        catch(const std::exception &verification){throw std::runtime_error(original+"; port verification: "+verification.what());}
        const auto remaining=usb_port_owner(listing,port,tcp_port,serial);
        if(remaining!=UsbPortOwner::absent && remaining!=UsbPortOwner::foreign)throw std::runtime_error(original+"; owned port removal not confirmed");
    }
    port=0;
}
}

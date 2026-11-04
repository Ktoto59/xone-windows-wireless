// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "owned_usb_port.hpp"
#include <algorithm>
#include <vector>
namespace xone {
struct UsbPortRecord {int port;std::string endpoint,serial;};
// Complete inventory only: a failed/truncated query must never authorize a new
// attach or turn an unknown result into proof that no import was created.
inline std::vector<UsbPortRecord> usb_port_inventory(std::string_view listing){
    std::vector<UsbPortRecord> result;
    if(listing.find_first_not_of(" \r\n\t")==listing.npos)return result;
    if(!listing.starts_with("Imported USB devices\n") && !listing.starts_with("Imported USB devices\r\n"))throw std::runtime_error("Invalid USBip inventory header");
    std::istringstream input{std::string(listing)};std::string line;bool endpoint=false,serial=false,mode=false;
    auto complete=[&]{if(!endpoint || !serial || !mode)throw std::runtime_error("Incomplete USBip port inventory");};
    while(std::getline(input,line)){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.starts_with("Port ")){
            if(!result.empty())complete();
            const auto colon=line.find(':',5);if(colon==line.npos)throw std::runtime_error("Invalid USBip inventory port");
            int port=0;const auto parsed=std::from_chars(line.data()+5,line.data()+colon,port);
            if(parsed.ec!=std::errc{} || parsed.ptr!=line.data()+colon || port<1 || port>65535 ||
               std::any_of(result.begin(),result.end(),[&](const auto &r){return r.port==port;}))throw std::runtime_error("Invalid/duplicate USBip inventory port");
            result.push_back({port,{},{}});endpoint=serial=mode=false;continue;
        }
        const auto start=line.find_first_not_of(" \t");if(start==line.npos || result.empty())continue;
        const auto text=std::string_view(line).substr(start);
        if(text.starts_with("-> usbip://")){
            if(endpoint)throw std::runtime_error("Duplicate USBip endpoint");
            result.back().endpoint=text.substr(3);endpoint=true;
        }else if(text.starts_with("-> serial: ")){
            if(serial)throw std::runtime_error("Duplicate USBip serial");
            result.back().serial=text.substr(11);serial=true;
        }else if(text.starts_with("-> mode: "))mode=true;
    }
    if(result.empty())throw std::runtime_error("Incomplete USBip inventory");
    complete();return result;
}
struct UsbAttachRecovery {
    bool pending=false,ambiguous=false;
    std::vector<UsbPortRecord> before;
    static std::string endpoint(unsigned tcp_port){return "usbip://127.0.0.1:"+std::to_string(tcp_port)+"/1-1";}
    template<class Run> void reconcile(int &port,unsigned tcp_port,Run run){
        if(!pending)return;
        std::string listing;run(L"port",false,&listing);
        const auto after=usb_port_inventory(listing);int candidate=0;
        for(const auto &record:after){
            if(record.endpoint!=endpoint(tcp_port) || !record.serial.empty())continue;
            if(std::any_of(before.begin(),before.end(),[&](const auto &r){return r.port==record.port;}))continue;
            if(candidate){ambiguous=true;throw std::runtime_error("Ambiguous USBip attach result; no port adopted");}
            candidate=record.port;
        }
        // Once evidence was ambiguous, one candidate disappearing does not
        // establish that the survivor belongs to this attempt.
        if(candidate && ambiguous)throw std::runtime_error("Previously ambiguous USBip attach result; no port adopted");
        // This is a cleanup record, never a successful/healthy attach. Detach
        // still revalidates the complete endpoint/bus/serial identity each time.
        port=candidate;pending=false;ambiguous=false;before.clear();
    }
    template<class Run> void attach(int &port,unsigned tcp_port,Run run){
        if(port || pending)throw std::runtime_error("USBip attach cleanup is still pending");
        std::string listing;run(L"port",false,&listing);before=usb_port_inventory(listing);
        if(std::any_of(before.begin(),before.end(),[&](const auto &r){return r.endpoint==endpoint(tcp_port);})){
            before.clear();throw std::runtime_error("USBip endpoint already imported; no foreign port adopted");
        }
        pending=true;
        try{
            const auto attached=run(L"-t "+std::to_wstring(tcp_port)+L" attach -r 127.0.0.1 -b 1-1 --once --terse",true,nullptr);
            if(attached<1 || attached>65535 || std::any_of(before.begin(),before.end(),[&](const auto &r){return r.port==attached;}))throw std::runtime_error("USBip returned a pre-existing/invalid port");
            port=attached;pending=false;before.clear();
        }catch(const std::exception &failure){
            const std::string original=failure.what();
            try{reconcile(port,tcp_port,run);}
            catch(const std::exception &verification){throw std::runtime_error(original+"; attach reconciliation: "+verification.what());}
            throw;
        }
    }
};
}

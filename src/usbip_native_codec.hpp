// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw_gip.hpp"
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
namespace xone::usbip {
struct Error:std::runtime_error {using std::runtime_error::runtime_error;};
struct Stall:Error {using Error::Error;};
struct Endpoint {uint8_t address,type,interval;uint16_t maximum;};
struct Profile {
    uint16_t vid,pid,revision;uint32_t speed;Bytes device,configuration,os_string;std::string product;
    std::map<std::pair<uint8_t,uint8_t>,std::vector<Endpoint>> settings;
    std::map<uint8_t,std::array<uint8_t,3>> classes;
    Profile(uint16_t product_id,uint16_t rev,uint32_t usb_speed,Bytes dev,Bytes config,Bytes os,std::string name);
    Bytes metadata()const;
    Bytes device_list()const;
};
Profile profile(uint16_t pid,bool audio);
struct Iso {uint32_t offset,length,actual=0;int32_t status=0;};
struct Request {
    uint32_t command=0,seq=0,direction=0,ep=0,flags=0;int32_t length=0,frame=0,count=0,interval=0;
    uint32_t unlink_target=0;std::array<uint8_t,8> setup{};Bytes data;std::vector<Iso> iso;
};
struct Completion {int32_t status=0,frame=0;uint32_t actual=0;Bytes data;std::vector<Iso> iso;};
std::optional<std::pair<Request,size_t>> request(std::span<const uint8_t> data);
Bytes completion(const Request &request,const Completion &result);
Completion failed(const Request &request,int32_t status);
Bytes unlinked(uint32_t seq,bool cancelled);
class Control {
    const Profile &profile_;uint8_t configured_=0;std::map<uint8_t,uint8_t> alternate_;
public:
    explicit Control(const Profile &profile);
    Bytes submit(const Request &request);
    const Endpoint *endpoint(uint8_t address)const;
};
}

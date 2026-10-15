// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbip_native_codec.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace xone::usbip {
namespace {
uint32_t be32(std::span<const uint8_t> b,size_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
uint16_t le16(std::span<const uint8_t> b,size_t at){return static_cast<uint16_t>(b[at]|uint16_t(b[at+1])<<8);}
void put32(Bytes &b,size_t at,uint32_t value){for(unsigned n=0;n<4;++n)b[at+n]=static_cast<uint8_t>(value>>(24-8*n));}
void put16(Bytes &b,size_t at,uint16_t value){b[at]=static_cast<uint8_t>(value>>8);b[at+1]=static_cast<uint8_t>(value);}
Bytes hex_bytes(std::string_view s){
    auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return static_cast<uint8_t>(c-'0');if(c>='a'&&c<='f')return static_cast<uint8_t>(c-'a'+10);throw Error("Invalid catalog hex");};
    if(s.size()%2)throw Error("Invalid catalog hex length");Bytes b(s.size()/2);
    for(size_t i=0;i<b.size();++i)b[i]=static_cast<uint8_t>(digit(s[2*i])*16+digit(s[2*i+1]));return b;
}
Bytes text_descriptor(std::string_view text){
    if(text.size()>126)throw Error("USB string exceeds descriptor limit");Bytes b{static_cast<uint8_t>(2+2*text.size()),3};
    for(unsigned char c:text){if(c>=128)throw Error("Expected ASCII catalog name");b.push_back(c);b.push_back(0);}return b;
}
}
Profile::Profile(uint16_t product_id,uint16_t rev,uint32_t usb_speed,Bytes dev,Bytes config,Bytes os,std::string name):
    vid(0x045e),pid(product_id),revision(rev),speed(usb_speed),device(std::move(dev)),configuration(std::move(config)),os_string(std::move(os)),product(std::move(name)){
    if(device.size()!=18 || device[0]!=18 || device[1]!=1 || device[17]!=1 ||
       le16(device,8)!=vid || le16(device,10)!=pid || le16(device,12)!=revision ||
       configuration.size()<9 || configuration[0]!=9 || configuration[1]!=2 || le16(configuration,2)!=configuration.size() ||
       os_string.size()<18 || os_string[0]!=os_string.size() || os_string[1]!=3 || !os_string[16])throw Error("Invalid native USB catalog");
    std::optional<std::pair<uint8_t,uint8_t>> current;uint8_t expected=0;
    auto finish=[&]{if(current && settings[*current].size()!=expected)throw Error("Incomplete endpoint catalog");};
    for(size_t at=9;at<configuration.size();){
        const auto n=configuration[at];if(n<2 || n>configuration.size()-at)throw Error("Truncated USB catalog");
        if(configuration[at+1]==4){
            finish();if(n!=9)throw Error("Invalid interface catalog");const auto number=configuration[at+2],alt=configuration[at+3];
            current={{number,alt}};if(settings.contains(*current))throw Error("Duplicate interface catalog");settings[*current]={};expected=configuration[at+4];
            const std::array<uint8_t,3> cls{configuration[at+5],configuration[at+6],configuration[at+7]};
            if(classes.contains(number) && classes[number]!=cls)throw Error("Conflicting interface class");classes[number]=cls;
        }else if(configuration[at+1]==5){
            if(n!=7 || !current)throw Error("Invalid endpoint catalog");const auto address=configuration[at+2];const auto type=configuration[at+3]&3;
            const auto maximum=le16(configuration,at+4);if(!(address&15) || address&0x70 || !maximum)throw Error("Invalid endpoint address/size");
            for(const auto &e:settings[*current])if(e.address==address)throw Error("Duplicate endpoint");
            settings[*current].push_back({address,static_cast<uint8_t>(type),configuration[at+6],maximum});
        }else throw Error("Unexpected catalog descriptor");at+=n;
    }
    finish();if(classes.size()!=configuration[4])throw Error("Interface count mismatch");
    for(const auto &[number,cls]:classes){(void)cls;if(!settings.contains({number,uint8_t(0)}))throw Error("Missing default alternate setting");}
}
Bytes Profile::metadata()const{
    Bytes b(312);const std::string path="/xone/virtual-headset/1-1",bus="1-1";
    std::copy(path.begin(),path.end(),b.begin());std::copy(bus.begin(),bus.end(),b.begin()+256);
    put32(b,288,1);put32(b,292,1);put32(b,296,speed);put16(b,300,vid);put16(b,302,pid);put16(b,304,revision);
    b[306]=device[4];b[307]=device[5];b[308]=device[6];b[309]=configuration[5];b[310]=device[17];b[311]=configuration[4];return b;
}
Bytes Profile::device_list()const{
    Bytes b(12);put16(b,0,0x0111);put16(b,2,5);put32(b,8,1);const auto d=metadata();b.insert(b.end(),d.begin(),d.end());
    for(const auto &[number,cls]:classes){(void)number;b.insert(b.end(),cls.begin(),cls.end());b.push_back(0);}return b;
}
// Generated only from the public, sanitized descriptor profiles.
#include "usbip_native_profiles.inc"
std::optional<std::pair<Request,size_t>> request(std::span<const uint8_t> b){
    if(b.size()<48)return {};Request r;r.command=be32(b,0);r.seq=be32(b,4);r.direction=be32(b,12);r.ep=be32(b,16);
    if(be32(b,8)!=0x10001 || r.direction>1 || r.ep>15)throw Error("Invalid USB/IP device/direction/endpoint");
    if(r.command==2){if(r.ep || r.direction)throw Error("Invalid unlink header");r.unlink_target=be32(b,20);return std::pair{r,size_t(48)};}
    if(r.command!=1)throw Error("Unsupported URB command");r.flags=be32(b,20);r.length=static_cast<int32_t>(be32(b,24));r.frame=static_cast<int32_t>(be32(b,28));r.count=static_cast<int32_t>(be32(b,32));r.interval=static_cast<int32_t>(be32(b,36));
    std::copy_n(b.begin()+40,8,r.setup.begin());
    if(r.length<0 || r.length>65536 || r.count<-1 || r.count>256 || r.interval<0)throw Error("URB exceeds bounds");
    if(!r.ep){if(r.count>0 || bool(r.setup[0]&128)!=bool(r.direction) || le16(r.setup,6)!=r.length)throw Error("Invalid control length/direction");}
    else if(std::any_of(r.setup.begin(),r.setup.end(),[](uint8_t x){return x!=0;}))throw Error("Setup on data endpoint");
    const size_t data_size=r.direction?0:static_cast<size_t>(r.length),count=r.count>0?static_cast<size_t>(r.count):0,total=48+data_size+16*count;
    if(b.size()<total)return {};r.data.assign(b.begin()+48,b.begin()+48+data_size);uint32_t end=0;
    for(size_t at=48+data_size;at<total;at+=16){
        Iso p{be32(b,at),be32(b,at+4),be32(b,at+8),static_cast<int32_t>(be32(b,at+12))};
        if(p.offset<end || p.offset>static_cast<uint32_t>(r.length) || p.length>static_cast<uint32_t>(r.length)-p.offset || p.actual || p.status)throw Error("Invalid ISO buffer range");
        end=p.offset+p.length;r.iso.push_back(p);
    }
    return std::pair{std::move(r),total};
}
Bytes completion(const Request &r,const Completion &c){
    if(c.actual>static_cast<uint32_t>(r.length) || c.data.size()!=(r.direction?c.actual:0) || c.iso.size()!=r.iso.size())throw Error("Invalid completion payload");
    uint32_t errors=0,total=0;
    for(size_t i=0;i<c.iso.size();++i){const auto &a=r.iso[i],&b=c.iso[i];if(a.offset!=b.offset || a.length!=b.length || b.actual>b.length)throw Error("Invalid ISO completion");errors+=b.status!=0;total+=b.actual;}
    if(!r.iso.empty() && total!=c.actual)throw Error("ISO actual mismatch");
    Bytes b(48);put32(b,0,3);put32(b,4,r.seq);put32(b,20,static_cast<uint32_t>(c.status));put32(b,24,c.actual);put32(b,28,static_cast<uint32_t>(c.frame));put32(b,32,static_cast<uint32_t>(r.count));put32(b,36,errors);
    b.insert(b.end(),c.data.begin(),c.data.end());
    for(const auto &p:c.iso){const auto at=b.size();b.resize(at+16);put32(b,at,p.offset);put32(b,at+4,p.length);put32(b,at+8,p.actual);put32(b,at+12,static_cast<uint32_t>(p.status));}return b;
}
Completion failed(const Request &r,int32_t status){Completion c;c.status=status;for(const auto &p:r.iso)c.iso.push_back({p.offset,p.length,0,status});return c;}
Bytes unlinked(uint32_t seq,bool cancelled){Bytes b(48);put32(b,0,4);put32(b,4,seq);put32(b,20,static_cast<uint32_t>(cancelled?-104:0));return b;}
Control::Control(const Profile &p):profile_(p){for(const auto &[number,cls]:p.classes){(void)cls;alternate_[number]=0;}}
const Endpoint *Control::endpoint(uint8_t address)const{
    if(!configured_)return nullptr;
    for(const auto &[number,alt]:alternate_)for(const auto &e:profile_.settings.at({number,alt}))if(e.address==address)return &e;return nullptr;
}
Bytes Control::submit(const Request &r){
    const auto &s=r.setup;const auto kind=s[0],code=s[1];const auto value=le16(s,2),index=le16(s,4),length=le16(s,6);Bytes b;const auto &p=profile_;
    if((kind&128)?!r.data.empty():r.data.size()!=length)throw Error("Control data mismatch");
    if(kind==0x80 && code==6){
        const auto descriptor=value>>8,number=value&255;
        if((descriptor==1 || descriptor==2) && number==0 && index==0)b=descriptor==1?p.device:p.configuration;
        else if(descriptor==3){
            if(!number && !index)b={4,3,9,4};else if(number==0xee && !index)b=p.os_string;
            else if(index==0x0409){
                if(number==p.device[14])b=text_descriptor("Microsoft");else if(number==p.device[15])b=text_descriptor(p.product);
                else if(number==p.device[16]){char serial[32]{};std::snprintf(serial,sizeof(serial),"XONE%04XTEST",static_cast<unsigned>(p.pid));b=text_descriptor(p.virtual_serial.empty()?serial:p.virtual_serial);}else throw Stall("Unknown string");
            }else throw Stall("Unknown string index");
        }else throw Stall("Unsupported descriptor");
    }else if(kind==0xc0 && code==p.os_string[16] && !value && index==4){b=Bytes(40);b[0]=40;b[5]=1;b[6]=4;b[8]=1;b[17]=1;const std::string compatible="XGIP10";std::copy(compatible.begin(),compatible.end(),b.begin()+18);}
    else if(!kind && code==9 && !index && !length && value<=1){configured_=static_cast<uint8_t>(value);for(auto &[number,alt]:alternate_){(void)number;alt=0;}}
    else if(kind==0x80 && code==8 && !value && !index)b={configured_};
    else if(kind==1 && code==11 && !length && configured_ && index<=255 && value<=255 && p.settings.contains({static_cast<uint8_t>(index),static_cast<uint8_t>(value)}))alternate_[static_cast<uint8_t>(index)]=static_cast<uint8_t>(value);
    else if(kind==0x81 && code==10 && !value && configured_ && index<=255 && alternate_.contains(static_cast<uint8_t>(index)))b={alternate_[static_cast<uint8_t>(index)]};
    else if(kind==0x80 && !code && !value && !index)b={static_cast<uint8_t>(bool(p.configuration[7]&64)),0};
    else if(kind==0x81 && !code && !value && configured_ && index<=255 && p.classes.contains(static_cast<uint8_t>(index)))b={0,0};
    else throw Stall("Unsupported USB control request");
    if(b.size()>length)b.resize(length);return b;
}
}

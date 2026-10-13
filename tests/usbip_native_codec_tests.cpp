// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbip_native_codec.hpp"
#include <iostream>
#include <functional>
using namespace xone;using namespace xone::usbip;
static void check(bool value,const char *message){if(!value)throw Error(message);}
static void put32(Bytes &b,size_t at,uint32_t value){for(unsigned n=0;n<4;++n)b[at+n]=static_cast<uint8_t>(value>>(24-8*n));}
static Request control(uint8_t kind,uint8_t code,uint16_t value,uint16_t index,uint16_t length){Request r;r.command=1;r.direction=(kind&128)!=0;r.length=length;r.setup={kind,code,static_cast<uint8_t>(value),static_cast<uint8_t>(value>>8),static_cast<uint8_t>(index),static_cast<uint8_t>(index>>8),static_cast<uint8_t>(length),static_cast<uint8_t>(length>>8)};return r;}
static void rejects(const std::function<void()> &f){bool failed=false;try{f();}catch(const Error &){failed=true;}check(failed,"Invalid USB/IP data accepted");}
int main(int argc,char **argv){
    try{
        if(argc==4 && std::string(argv[1])=="dump"){
            const auto pid=static_cast<uint16_t>(std::stoul(argv[2],nullptr,16));auto p=profile(pid,std::string(argv[3])=="audio");Control c(p);
            std::cout<<"metadata\t"<<hex(p.metadata())<<"\nlist\t"<<hex(p.device_list())<<'\n';
            const Request queries[]={control(0x80,6,0x0100,0,255),control(0x80,6,0x0200,0,255),control(0x80,6,0x03ee,0,255),control(0xc0,p.os_string[16],0,4,255),control(0x80,6,static_cast<uint16_t>(0x0300|p.device[14]),0x0409,255),control(0x80,6,static_cast<uint16_t>(0x0300|p.device[15]),0x0409,255),control(0x80,6,static_cast<uint16_t>(0x0300|p.device[16]),0x0409,255)};
            unsigned i=0;for(const auto &q:queries)std::cout<<"control"<<i++<<'\t'<<hex(c.submit(q))<<'\n';return 0;
        }
        unsigned passed=0;auto test=[&](const char *name,const std::function<void()> &f){f();++passed;std::cout<<"PASS "<<name<<'\n';};
        test("All public models preserve USB controls and alternate state",[]{
            for(uint16_t pid:{uint16_t(0x0b32),uint16_t(0x0b17),uint16_t(0x0b12)}){
                auto p=profile(pid,pid!=0x0b12);Control c(p);check(p.metadata().size()==312,"Wrong USB/IP metadata length");check(!c.endpoint(p.settings.at({uint8_t(0),uint8_t(0)})[0].address),"Unconfigured endpoint was active");
                check(c.submit(control(0x80,6,0x0100,0,8))==Bytes(p.device.begin(),p.device.begin()+8),"Descriptor truncation failed");
                c.submit(control(0,9,1,0,0));check(c.endpoint(p.settings.at({uint8_t(0),uint8_t(0)})[0].address)!=nullptr,"Configuration did not enable endpoints");
                c.submit(control(1,11,1,1,0));check(c.submit(control(0x81,10,0,1,1))==Bytes{1},"Alternate setting not retained");
                rejects([&]{c.submit(control(0x40,0x99,0,0,0));});c.submit(control(0,9,0,0,0));check(!c.endpoint(0x81),"Reset left endpoint active");
            }
        });
        test("Incremental OUT framing waits for every payload byte",[]{
            Bytes b(52);put32(b,0,1);put32(b,4,7);put32(b,8,0x10001);put32(b,16,1);put32(b,24,4);b[48]=4;
            for(size_t n=0;n<b.size();++n)check(!request(std::span(b).first(n)),"Accepted truncated transfer");const auto parsed=request(b);check(parsed && parsed->second==52 && parsed->first.data==Bytes({4,0,0,0}),"Incorrect OUT payload");
        });
        test("ISO bounds and completion sums are enforced",[]{
            Bytes b(80);put32(b,0,1);put32(b,4,3);put32(b,8,0x10001);put32(b,12,1);put32(b,16,2);put32(b,24,512);put32(b,32,2);put32(b,52,256);put32(b,64,256);put32(b,68,256);
            const auto parsed=request(b);check(parsed && parsed->first.iso.size()==2,"Valid ISO rejected");const auto &r=parsed->first;
            auto c=failed(r,-19);check(completion(r,c).size()==80,"Failed ISO lost descriptors");c.actual=1;rejects([&]{completion(r,c);});put32(b,64,128);rejects([&]{request(b);});
        });
        test("Malformed headers and signed bounds fail explicitly",[]{
            Bytes b(48);put32(b,0,1);put32(b,8,0x10001);put32(b,16,1);put32(b,24,0xffffffff);rejects([&]{request(b);});put32(b,24,65537);rejects([&]{request(b);});put32(b,24,0);put32(b,12,2);rejects([&]{request(b);});
        });
        test("UNLINK ignores padding and replaces submit completion",[]{
            Bytes b(48,0xaa);put32(b,0,2);put32(b,4,9);put32(b,8,0x10001);put32(b,12,0);put32(b,16,0);put32(b,20,7);
            const auto r=request(b);check(r && r->first.unlink_target==7,"Valid padded UNLINK rejected");check(unlinked(9,true).size()==48 && unlinked(9,false)!=unlinked(9,true),"Unlink status incorrect");
        });
        std::cout<<passed<<" native codec tests passed\n";return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

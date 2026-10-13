// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbip_native_core.hpp"
#include "gip_link.hpp"
#include <iostream>
using namespace xone;
using namespace xone::usbip;
void require(bool value){if(!value)throw Error("Native core assertion failed");}
Request control(uint32_t seq,std::array<uint8_t,8> setup){Request r;r.command=1;r.seq=seq;r.setup=setup;r.direction=bool(setup[0]&128);r.length=setup[6]|(int32_t(setup[7])<<8);return r;}
void configured(Core &c){c.imported();c.submit(control(1,{0,9,1,0,0,0,0,0}));c.submit(control(2,{1,11,1,0,1,0,0,0}));c.replies();}
Bytes capture_packet(){Bytes pcm(386);pcm[1]=6;for(size_t i=2;i<pcm.size();++i)pcm[i]=static_cast<uint8_t>(i);return encode_gip({0x60,32,1,0,pcm});}
Request iso_request(uint32_t seq,bool incoming){Request r;r.command=1;r.seq=seq;r.ep=2;r.direction=incoming;r.count=8;r.frame=42;for(uint32_t i=0;i<8;++i)r.iso.push_back({i*256,256,0,0});r.length=2048;if(!incoming){r.data.resize(2048);for(uint32_t i=0;i<8;++i){const auto frame=encode_gip({0x60,32,static_cast<uint8_t>(i+1),0,Bytes(192,static_cast<uint8_t>(i+1))});std::copy(frame.begin(),frame.end(),r.data.begin()+i*256);r.iso[i].length=static_cast<uint32_t>(frame.size());}}return r;}
int main(){try{
    auto p=profile(0x0b32,true);Core c(p,true);const Bytes mac{1,2,3,4,5,6};c.radio(1,2,mac);configured(c);
    c.submit(iso_request(10,true));require(c.replies().empty());const auto wire=capture_packet();c.radio(4,2,wire);
    const auto replies=c.replies();require(replies.size()==1 && replies[0].size()==48+432+128);const auto frames=parse_gip(std::span<const uint8_t>(replies[0]).subspan(48,432));require(frames.size()==8 && frames.front().payload.size()==50 && frames.front().payload[2]==2 && frames.back().payload.back()==129);
    c.submit(iso_request(11,false));require(c.replies().empty());auto tx=c.outbound();require(tx.size()==1 && tx[0].audio && tx[0].data.size()==1542);const auto parsed=parse_gip(tx[0].data);require(parsed[0].payload.size()==1536 && parsed[0].payload[0]==1 && parsed[0].payload[1535]==8);
    c.complete(2,tx[0].token,0);require(c.replies().size()==1 && c.stats().audio_tx==1);
    Request irq;irq.command=1;irq.seq=20;irq.direction=1;irq.ep=1;irq.length=64;c.submit(irq);Request unlink;unlink.command=2;unlink.seq=21;unlink.unlink_target=20;c.submit(unlink);require(c.replies().size()==1);c.radio(2,2,Bytes{3,32,1,4,0,0,0,0});require(c.replies().empty());
    for(unsigned i=0;i<20;++i)c.radio(4,2,wire);require(c.stats().capture_high_water==64 && c.stats().dropped>0);
    c.submit(irq);c.radio(3,2,{});require(!c.ready() && c.replies().size()==1);c.complete(2,tx[0].token,0);require(c.replies().empty());
    GipPacket large{4,static_cast<uint8_t>(32|16|64|128),7,112,Bytes(112,42)};const auto parts=interrupt_packets(encode_gip(large));require(parts.size()==2 && parts[0].size()<=64 && parts[1].size()<=64);const auto a=parse_gip(parts[0])[0],b=parse_gip(parts[1])[0];require((a.options&64) && !(a.options&16) && a.chunk_offset==112 && !(b.options&64) && (b.options&16) && b.chunk_offset==56);
    std::cout<<"PASS: physical audio clock, eight capture slots, render TX acknowledgements, bounded FIFO, UNLINK, generation reset and GIP chunk ACKs\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbip_native_core.hpp"
#include "gip_link.hpp"
#include <iostream>
#include <source_location>
using namespace xone;
using namespace xone::usbip;
void require(bool value,const std::source_location location=std::source_location::current()){if(!value)throw Error("Native core assertion failed at line "+std::to_string(location.line()));}
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
    for(unsigned i=0;i<20;++i)c.radio(4,2,wire);require(c.stats().capture_high_water==32 && c.stats().dropped>0);
    c.submit(irq);c.radio(3,2,{});require(!c.ready() && c.replies().size()==1);c.complete(2,tx[0].token,0);require(c.replies().empty());
    GipPacket large{4,static_cast<uint8_t>(32|16|64|128),7,112,Bytes(112,42)};const auto parts=interrupt_packets(encode_gip(large));require(parts.size()==2 && parts[0].size()<=64 && parts[1].size()<=64);const auto a=parse_gip(parts[0])[0],b=parse_gip(parts[1])[0];require((a.options&64) && !(a.options&16) && a.chunk_offset==112 && !(b.options&64) && (b.options&16) && b.chunk_offset==56);
    auto old_profile=profile(0x0b17,true);Core old(old_profile,true);const auto start=Core::Clock::now();old.radio(1,1,mac,start);configured(old);
    old.radio(4,1,wire,start);old.submit(iso_request(40,true),start);require(old.replies().empty());old.tick(start+std::chrono::milliseconds(8));require(old.replies().size()==1);
    auto render=iso_request(41,false);render.ep=1;old.submit(render,start+std::chrono::milliseconds(8));auto first=old.outbound();require(first.size()==1);old.complete(1,first[0].token,0,start+std::chrono::milliseconds(8));old.replies();
    render.seq=42;old.submit(render,start+std::chrono::milliseconds(9));require(old.outbound().empty());old.tick(start+std::chrono::milliseconds(16));require(old.outbound().size()==1);
    Request mode;mode.command=1;mode.seq=50;mode.ep=10;mode.data={5,32,2,15,6,0,0,0,0,0,0,0x55,0x53,0,0,0,0,0,0};mode.length=static_cast<int32_t>(mode.data.size());old.submit(mode,start+std::chrono::milliseconds(17));require(old.stats().mode_suppressed==1 && old.outbound().empty());old.replies();
    old.tick(start+std::chrono::milliseconds(500));old.submit(iso_request(51,true),start+std::chrono::milliseconds(500));require(old.replies().empty());
    auto fresh=profile(0x0b32,true);Core stalled(fresh,true);stalled.radio(1,1,mac,start);configured(stalled);stalled.radio(4,1,wire,start);auto r=iso_request(60,false);stalled.submit(r,start);auto out=stalled.outbound();require(out.size()==1);stalled.complete(1,out[0].token,0,start);stalled.replies();r.seq=61;stalled.submit(r,start+std::chrono::milliseconds(40));require(stalled.outbound().empty());stalled.tick(start+std::chrono::milliseconds(105));require(stalled.stats().render_expired==1 && stalled.outbound().empty());
    Core late(fresh,true);late.radio(1,1,mac,start);configured(late);late.radio(4,1,wire,start);late.submit(iso_request(70,false),start);out=late.outbound();
    require(out.size()==1 && out[0].media_deadline==start+std::chrono::milliseconds(64));const auto generation=late.stats().generation;
    late.complete(1,out[0].token,-18,start+std::chrono::milliseconds(65));
    require(late.ready() && !late.stats().recovery_required && late.stats().generation==generation && late.stats().render_expired==1 && late.replies().size()==1);
    late.radio(4,1,wire,start+std::chrono::milliseconds(66));late.submit(iso_request(71,false),start+std::chrono::milliseconds(66));out=late.outbound();require(out.size()==1);
    late.complete(1,out[0].token,0,start+std::chrono::milliseconds(66));require(late.stats().audio_tx==1 && late.ready());
    for(const auto pid:{uint16_t(0x0b17),uint16_t(0x0b32)}){
        const auto prof=profile(pid,true);Core reconnect(prof,true);Bytes body(28);body[8]=0x5e;body[9]=4;body[10]=static_cast<uint8_t>(pid);body[11]=static_cast<uint8_t>(pid>>8);
        const auto announce=encode_gip({2,32,7,0,body});const auto stale=encode_gip({3,32,9,0,{1,2,3,4}});
        reconnect.radio(1,1,mac,start);reconnect.radio(2,1,announce,start);configured(reconnect);
        Request input;input.command=1;input.direction=1;input.ep=pid==0x0b17?6:1;input.length=64;input.seq=80;reconnect.submit(input,start);
        auto read=reconnect.replies();require(read.size()==1 && Bytes(read[0].begin()+48,read[0].end())==announce);
        require(reconnect.initialization().configurations==1 && reconnect.initialization().interfaces==1 && reconnect.initialization().irq_in==1 && reconnect.initialization().irq_delivered==1);
        reconnect.radio(2,1,stale,start);reconnect.detached();
        require(reconnect.initialization().irq_delivered==1);
        const auto dropped=reconnect.stats().dropped;
        for(unsigned i=0;i<500;++i)reconnect.radio(4,1,wire,start+std::chrono::milliseconds(i));
        require(reconnect.stats().dropped==dropped && reconnect.stats().capture_without_usb==4000);
        configured(reconnect);require(reconnect.initialization().irq_in==0 && reconnect.initialization().irq_delivered==0);
        input.seq=81;reconnect.submit(input,start+std::chrono::seconds(1));read=reconnect.replies();
        require(read.size()==1 && Bytes(read[0].begin()+48,read[0].end())==announce && reconnect.stats().announce_replays==1);
        input.seq=82;reconnect.submit(input,start+std::chrono::seconds(1));require(reconnect.replies().empty());
        reconnect.radio(2,1,stale,start+std::chrono::seconds(1));read=reconnect.replies();require(read.size()==1 && Bytes(read[0].begin()+48,read[0].end())==stale);
        const auto reports=reconnect.stats().capture_drop_reports;
        for(unsigned i=0;i<500;++i)reconnect.radio(4,1,wire,start+std::chrono::seconds(2)+std::chrono::milliseconds(i));
        require(reconnect.stats().dropped>dropped && reconnect.stats().capture_drop_reports-reports<=1);
    }
    std::cout<<"PASS: RF audio, real TX ACK, bounded media, generation reset, 2021 clock, isolated expiry, clean identity-only reimport and coalesced drops\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

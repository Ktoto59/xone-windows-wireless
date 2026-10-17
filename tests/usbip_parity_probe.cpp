// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic differential harness. No WinUSB, physical devices or installed changes.
#include "usbip_native_core.hpp"
#include "gip_link.hpp"
#include <iostream>
using namespace xone;
using namespace xone::usbip;
using namespace std::chrono_literals;
Request control(uint32_t seq,std::array<uint8_t,8> setup){Request r;r.command=1;r.seq=seq;r.setup=setup;r.direction=bool(setup[0]&128);return r;}
Request render(uint32_t seq,unsigned count=8){
    Request r;r.command=1;r.seq=seq;r.ep=2;r.frame=42;r.count=static_cast<int32_t>(count);
    for(unsigned i=0;i<count;++i){auto b=encode_gip({0x60,32,1,0,Bytes(192,77)});r.iso.push_back({static_cast<uint32_t>(r.data.size()),static_cast<uint32_t>(b.size()),0,0});r.data.insert(r.data.end(),b.begin(),b.end());}
    r.length=static_cast<int32_t>(r.data.size());return r;
}
Bytes audio(){Bytes p(386,22);p[0]=0;p[1]=6;return encode_gip({0x60,32,1,0,p});}
uint32_t be(const Bytes &b,size_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    try{
        auto p=profile(0x0b32,true);Core core(p,true);auto t=Core::Clock::time_point(100s);Bytes mac{2,0,0,0,0,1};
        core.radio(1,1,mac,t);core.imported();core.submit(control(1,{0,9,1,0,0,0,0,0}),t);core.submit(control(2,{1,11,1,0,1,0,0,0}),t);core.replies();core.radio(4,1,audio(),t);
        std::string mode=argv[1];
        if(mode=="error_ack" || mode=="ok_ack" || mode=="recover_ack" || mode=="ack_timeout" || mode=="expired_ack"){
            core.submit(render(10),t);auto tx=core.outbound();if(tx.size()!=1)return 3;
            if(mode=="ack_timeout")core.tick(t+5001ms);else core.complete(1,tx[0].token,mode=="ok_ack"?0:mode=="expired_ack"?-18:-5,t);
            auto replies=core.replies();if(replies.size()!=1)return 4;
            if(mode=="recover_ack"){
                if(core.ready() || !core.associated() || !core.stats().recovery_required)return 11;
                core.imported();core.submit(control(1,{0,9,1,0,0,0,0,0}),t+1ms);core.submit(control(2,{1,11,1,0,1,0,0,0}),t+1ms);core.replies();
                // No RF reassociation is needed. Old ACK cannot affect the new import.
                core.complete(1,tx[0].token,0,t+1ms);core.radio(4,1,audio(),t+1ms);core.submit(render(20),t+1ms);auto next=core.outbound();if(next.size()!=1)return 12;
                core.complete(1,next[0].token,0,t+1ms);replies=core.replies();if(replies.size()!=1)return 13;
            }
            std::cout<<"{\"ready\":"<<(core.ready()?"true":"false")<<",\"generation\":"<<core.stats().generation<<",\"wcid\":"<<unsigned(core.stats().wcid)<<",\"recovery\":"<<(core.stats().recovery_required?"true":"false")<<",\"status\":"<<static_cast<int32_t>(be(replies[0],20));
            if(mode=="expired_ack")std::cout<<",\"expired\":"<<core.stats().render_expired<<",\"iso_status\":"<<static_cast<int32_t>(be(replies[0],60));
            std::cout<<"}\n";
        }else{
            // Consume the initial render credit; keep the same live RF session.
            core.submit(render(10),t);auto tx=core.outbound();if(tx.size()!=1)return 5;core.complete(1,tx[0].token,0,t);core.replies();
            if(mode=="expiry"){
                core.submit(render(11),t+40ms);core.tick(t+105ms);auto replies=core.replies();if(replies.size()!=1)return 6;
                std::cout<<"{\"status\":"<<static_cast<int32_t>(be(replies[0],20))<<",\"iso_status\":"<<static_cast<int32_t>(be(replies[0],60))<<",\"frame\":"<<be(replies[0],28)<<",\"tx\":"<<core.outbound().size()<<"}\n";
            }else if(mode=="pcm_age"){
                core.submit(render(11,4),t);if(core.replies().size()!=1)return 7;
                core.submit(render(12,4),t+60ms);if(!core.outbound().empty())return 8;
                core.radio(4,1,audio(),t+70ms);auto old_tx=core.outbound();
                auto done=core.replies();if(done.size()!=1)return 10;
                std::cout<<"{\"tx\":"<<old_tx.size()<<",\"expired\":"<<core.stats().render_expired<<",\"oldest_pcm_age_ms\":70,\"status\":"<<static_cast<int32_t>(be(done[0],20))<<",\"iso_status\":"<<static_cast<int32_t>(be(done[0],60))<<",\"actual\":"<<be(done[0],24)<<"}\n";
            }else return 9;
        }
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbip_native_core.hpp"
#include "gip_link.hpp"
#include <iostream>
#include <source_location>
using namespace xone;
using namespace xone::usbip;
using namespace std::chrono_literals;
namespace {
void require(bool value,const std::source_location where=std::source_location::current()){
    if(!value)throw Error("Audio recovery assertion failed at line "+std::to_string(where.line()));
}
Request setup(uint32_t seq,std::array<uint8_t,8> bytes){Request r;r.command=1;r.seq=seq;r.setup=bytes;return r;}
void connect(Core &c,Core::Clock::time_point now){
    c.radio(1,1,Bytes{1,2,3,4,5,6},now);c.imported();
    c.submit(setup(1,{0,9,1,0,0,0,0,0}),now);
    c.submit(setup(2,{1,11,1,0,1,0,0,0}),now);c.replies();
}
Bytes microphone(){Bytes data(386);data[1]=6;return encode_gip({0x60,32,1,0,data});}
Request render(uint32_t seq,size_t frames,uint8_t value,uint32_t ep=2){
    Request r;r.command=1;r.seq=seq;r.ep=ep;r.count=static_cast<int32_t>(frames);
    for(size_t i=0;i<frames;++i){
        auto frame=encode_gip({0x60,32,static_cast<uint8_t>(i+1),0,Bytes(192,value)});
        const auto offset=static_cast<uint32_t>(r.data.size()),length=static_cast<uint32_t>(frame.size());
        r.data.insert(r.data.end(),frame.begin(),frame.end());r.iso.push_back({offset,length,0,0});
    }
    r.length=static_cast<int32_t>(r.data.size());return r;
}
RawGipTransmit take(Core &c){auto out=c.outbound();require(out.size()==1 && out[0].audio);return std::move(out[0]);}
Bytes payload(const RawGipTransmit &tx){auto p=parse_gip(tx.data);require(p.size()==1 && p[0].payload.size()==1536);return p[0].payload;}
void unlink(Core &c,uint32_t seq,uint32_t target,Core::Clock::time_point now){Request r;r.command=2;r.seq=seq;r.unlink_target=target;c.submit(r,now);c.replies();}
}
int main(){try{
    const auto now=Core::Clock::time_point{}+100s;
    const auto profile2024=profile(0x0b32,true),profile2021=profile(0x0b17,true);
    const auto mic=microphone();
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(10,8,1),now);auto tx=take(c);c.complete(1,tx.token,0,now);c.replies();
        // No new microphone arrivals: predict just three 8 ms sends, never
        // extend the prediction window by sending or receiving USB requests.
        for(unsigned i=1;i<=3;++i){
            c.submit(render(10+i,8,static_cast<uint8_t>(i+1)),now+1ms+(i-1)*8ms);
            require(c.outbound().empty());require(c.wake_at(now+(i-1)*8ms)==now+i*8ms);
            c.tick(now+i*8ms);tx=take(c);c.complete(1,tx.token,0,now+i*8ms);c.replies();
            require(c.outbound().empty());
        }
        require(c.stats().render_predicted==3);
        c.submit(render(20,8,5),now+25ms);c.tick(now+32ms);require(c.outbound().empty());
        require(c.wake_at(now+32ms)>now+32ms); // No zero-delay polling after the horizon.
        c.radio(4,1,mic,now+40ms);tx=take(c);require(c.stats().max_radio_gap_us==40000);
        c.complete(1,tx.token,0,now+40ms);require(c.ready() && !c.stats().recovery_required);
    }
    // Virtual-time fault injection; no sleeps, real adapter, drivers or files.
    for(auto pause:{10ms,20ms,40ms,80ms,250ms,500ms}){
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(30,8,1),now);auto first=take(c);
        c.submit(render(31,8,2),now+1ms);c.submit(render(32,8,3),now+1ms);
        const auto generation=c.stats().generation;
        // Both the callback and the new RX batch have been held by a worker stall.
        c.radio(4,1,mic,now+pause);c.complete(1,first.token,0,now+pause);c.replies();
        if(pause>=64ms){
            require(c.outbound().empty() && c.stats().render_expired==2);
            c.submit(render(33,8,4),now+pause);auto fresh=take(c);
            const auto pcm=payload(fresh);require(pcm.front()==4 && pcm.back()==4);
            c.complete(1,fresh.token,0,now+pause);
        }else{
            auto second=take(c);require(payload(second).front()==2);
            c.complete(1,second.token,0,now+pause);require(c.outbound().empty());
            c.tick(now+pause);require(c.outbound().empty());
            c.tick(now+pause+8ms);auto third=take(c);require(payload(third).front()==3);
            c.complete(1,third.token,0,now+pause+8ms);
        }
        require(c.ready() && c.stats().generation==generation && !c.stats().recovery_required);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(25,32,6),now);auto tx=take(c);c.complete(1,tx.token,0,now);
        for(unsigned i=1;i<4;++i){
            const auto wake=c.wake_at(now+(i-1)*8ms+100us);
            require(wake && *wake>=now+i*8ms-300us && *wake<=now+i*8ms+300us);
            c.radio(4,1,mic,now+i*8ms+100us);tx=take(c);
            c.complete(1,tx.token,0,now+i*8ms+100us);
        }
        require(c.stats().render_rephased==0 && c.replies().size()==1);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(40,2,1),now);require(c.replies().size()==1); // Old accepted fragment.
        c.submit(render(41,6,2),now+63ms);require(c.outbound().empty());
        c.tick(now+64ms); // Only the old two frames have expired.
        require(c.stats().render_dropped_frames==2 && c.stats().render_expired==0);
        require(c.replies().size()==1);
        c.radio(4,1,mic,now+65ms);c.submit(render(42,2,3),now+65ms);auto tx=take(c);
        const auto pcm=payload(tx);require(pcm[0]==2 && pcm[6*192-1]==2 && pcm[6*192]==3 && pcm.back()==3);
        require(tx.media_deadline==now+127ms && c.stats().render_high_water<=32);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(50,2,7),now);c.replies();
        // A previously accepted sequence number can be reused and unlinked.
        // Unlink must remove only the six newly staged frames, preserving seven's PCM.
        c.submit(render(50,6,8),now+40ms);require(c.outbound().empty());
        unlink(c,51,50,now+41ms);c.radio(4,1,mic,now+42ms);
        c.submit(render(52,6,9),now+42ms);auto tx=take(c);const auto pcm=payload(tx);
        require(pcm[0]==7 && pcm[2*192-1]==7 && pcm[2*192]==9 && pcm.back()==9);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(60,2,1),now);c.replies();
        c.submit(render(61,6,2),now+10ms);auto old=take(c);
        // Unsent TX expiry of an aggregate with an old constituent must retain
        // newer accepted PCM belonging to an independent request.
        c.complete(1,old.token,-18,now+64ms);c.replies();
        c.submit(render(62,4,3),now+64ms);c.replies();
        c.radio(4,1,mic,now+65ms);c.submit(render(63,4,4),now+65ms);auto fresh=take(c);
        const auto pcm=payload(fresh);require(pcm[0]==3 && pcm[4*192]==4 && c.ready());
    }
    {
        Core c(profile2021,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(70,8,1,1),now);auto first=take(c);
        c.submit(render(71,8,2,1),now+1ms);c.submit(render(72,8,3,1),now+1ms);
        c.complete(1,first.token,0,now+40ms);auto second=take(c);
        c.complete(1,second.token,0,now+40ms);require(c.outbound().empty());
        c.tick(now+48ms);require(take(c).audio && c.stats().render_rephased==1);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(65,2,1),now);c.replies();
        c.submit(render(66,14,2),now+60ms);require(c.outbound().empty());
        c.radio(4,1,mic,now+63ms);auto mixed=take(c);require(mixed.media_deadline==now+64ms);
        c.complete(1,mixed.token,-18,now+65ms);require(c.replies().empty() && c.outbound().empty());
        c.tick(now+71ms);auto tail=take(c);const auto pcm=payload(tail);
        require(pcm.front()==2 && pcm.back()==2 && tail.media_deadline==now+124ms);
        c.complete(1,tail.token,0,now+71ms);auto done=c.replies();require(done.size()==1);
        const auto be=[&](size_t at){return uint32_t(done[0][at])<<24|uint32_t(done[0][at+1])<<16|uint32_t(done[0][at+2])<<8|done[0][at+3];};
        require(be(24)==8*198 && c.stats().render_expired==1 && c.stats().render_dropped_frames==8);
        for(size_t i=0;i<14;++i)require(static_cast<int32_t>(be(48+i*16+12))==(i<6?-18:0));
        require(c.ready() && !c.stats().recovery_required);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(75,32,5),now);
        for(unsigned i=0;i<4;++i){
            if(i)c.tick(now+i*8ms);
            auto tx=take(c);c.complete(1,tx.token,(i==0 || i==3)?-18:0,now+i*8ms);
            require(c.outbound().empty());
        }
        auto done=c.replies();require(done.size()==1);
        const auto be=[&](size_t at){return uint32_t(done[0][at])<<24|uint32_t(done[0][at+1])<<16|uint32_t(done[0][at+2])<<8|done[0][at+3];};
        require(be(24)==16*198 && c.stats().render_expired==1 && c.stats().render_dropped_frames==16);
        for(size_t i=0;i<32;++i)require(static_cast<int32_t>(be(48+i*16+12))==((i<8 || i>=24)?-18:0));
        require(c.ready());
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(80,8,1),now);auto tx=take(c);const auto generation=c.stats().generation;
        c.complete(1,tx.token,-19,now+10ms); // A real transport failure still recovers.
        require(c.stats().recovery_required && c.stats().generation>generation);
        c.radio(3,1,{},now+11ms);require(!c.associated());
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(90,8,1),now);auto tx=take(c);c.complete(1,tx.token,0,now);c.replies();
        c.radio(4,1,mic,now+38ms,now+8ms); // 30 ms mailbox delay, not new RX.
        c.submit(render(91,8,2),now+39ms);tx=take(c);c.complete(1,tx.token,0,now+39ms);c.replies();
        c.submit(render(92,8,3),now+47ms);require(c.outbound().empty());
        c.radio(4,1,mic,now+47ms,now+8ms);require(c.stats().stale_audio==1 && c.outbound().empty());
        c.radio(4,1,mic,now+48ms,now+48ms);require(take(c).audio);
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(92,32,4),now);auto tx=take(c);c.complete(1,tx.token,0,now+1ms);
        c.tick(now+64ms);auto done=c.replies();require(done.size()==1 && c.stats().audio_tx==1);
        const auto be=[&](size_t at){return uint32_t(done[0][at])<<24|uint32_t(done[0][at+1])<<16|uint32_t(done[0][at+2])<<8|done[0][at+3];};
        require(be(24)==8*198 && be(36)==24);
        for(size_t i=0;i<32;++i)require(static_cast<int32_t>(be(48+i*16+12))==(i<8?0:-18));
    }
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);const auto generation=c.stats().generation;
        auto bad=parse_gip(mic)[0];bad.payload[1]=7;const auto invalid=encode_gip(bad);
        Bytes mixed;for(unsigned i=0;i<9;++i){mixed.insert(mixed.end(),invalid.begin(),invalid.end());mixed.insert(mixed.end(),mic.begin(),mic.end());}
        c.radio(4,1,mixed,now);require(c.ready() && c.stats().audio_resyncs==0);
        const auto malformed=c.stats().malformed_audio;
        c.radio(4,1,invalid,now+1ms);
        require(c.ready() && c.stats().generation==generation && c.stats().malformed_audio==malformed+1);
        c.radio(4,1,mic,now+8ms);require(c.ready());
        for(unsigned i=0;i<8;++i)c.radio(4,1,invalid,now+10ms+i*1ms);
        require(c.associated() && !c.ready() && c.stats().recovery_required && c.stats().audio_resyncs==1);
        c.imported();c.submit(setup(1,{0,9,1,0,0,0,0,0}),now+20ms);c.replies();
        for(unsigned i=0;i<8;++i)c.radio(4,1,invalid,now+21ms+i*1ms);
        require(c.ready() && c.stats().audio_resyncs==1); // Resync cooldown survives USB import.
        c.radio(4,1,mic,now+30ms);require(c.stats().layout);
        c.radio(4,1,Bytes{0x60},now+31ms);require(c.ready()); // Bad framing stays media-local.
        bool rejected=false;try{c.radio(2,1,Bytes{0x60},now+32ms);}catch(const Error &){rejected=true;}
        require(rejected); // Reliable/control corruption is not silently hidden.
    }
    for(const auto rx_period:{8050us,7950us}){
        Core c(profile2024,true);connect(c,now);
        auto rx=0ns,usb=0ns,at=0ns;uint32_t seq=1000;unsigned iterations=0;
        while(at<80s){
            require(++iterations<100000);
            auto next=std::min(rx,usb);
            if(const auto wake=c.wake_at(now+at))next=std::min(next,std::max(at+1ns,std::chrono::duration_cast<std::chrono::nanoseconds>(*wake-now)));
            at=next;if(at>=80s)break;
            if(rx==at){c.radio(4,1,mic,now+at);rx+=rx_period;}
            if(usb==at){c.submit(render(seq++,8,5),now+at);usb+=8000us;}
            c.tick(now+at);
            for(auto &tx:c.outbound())c.complete(1,tx.token,0,now+at);
            c.replies();
        }
        const auto sent=static_cast<int64_t>(c.stats().audio_tx),received=static_cast<int64_t>(c.stats().audio_rx);
        // A faster receiver may legitimately run out of USB input. A slower
        // receiver must not accumulate the old 62 extra packets over 80 s.
        if(rx_period>8000us)require(sent-received<=3 && received-sent<=3);
        require(sent<=received+3 && c.ready() && c.stats().render_high_water<=32);
    }
    std::cout<<"PASS: bounded prediction, 10/20/40/80/250/500 ms stalls, no catch-up bursts, per-frame expiry, unlink ownership and transport recovery\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

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
Core::Stats bursty_render_stream(std::span<const std::chrono::nanoseconds> gaps,size_t batch=1,std::chrono::microseconds dispatch_jitter=0us,std::chrono::microseconds callback_delay=100us,unsigned queue_depth=8){
    const auto start=Core::Clock::time_point{}+1h;const auto p=profile(0x0b32,true);
    Bytes mic;for(size_t n=0;n<batch;++n){const auto packet=microphone();mic.insert(mic.end(),packet.begin(),packet.end());}
    Core c(p,true);connect(c,start);c.radio(4,1,mic,start);
    uint32_t seq=1000;for(unsigned n=0;n<queue_depth;++n)c.submit(render(seq++,8,5),start);
    auto tx=take(c);auto completion=callback_delay+0ns;auto rx=gaps.front(),at=0ns;
    size_t gap=1;unsigned iterations=0;auto last_sent=0ns;
    for(;;){
        require(++iterations<100000);
        auto next=std::min(rx,completion);
        if(const auto wake=c.wake_at(start+at))next=std::min(next,std::max(at+1ns,std::chrono::duration_cast<std::chrono::nanoseconds>(*wake-start)));
        at=next+((iterations%17==0)?dispatch_jitter:0us);if(at>=30s)break;
        while(rx<=at){c.radio(4,1,mic,start+at,start+rx);rx+=gaps[gap++%gaps.size()];}
        if(completion<=at){completion=60s;c.complete(1,tx.token,0,start+at);}
        c.tick(start+at);
        // Windows keeps eight ISO requests queued with observed peaks at ten.
        // Complete physical
        // sends after a short real completion delay and replace each finished
        // USB request immediately. Keep source RX time across dispatch jitter.
        for(const auto &reply:c.replies()){(void)reply;c.submit(render(seq++,8,5),start+at);}
        for(auto &out:c.outbound()){
            require(completion==60s && at-last_sent>=7500us-dispatch_jitter);last_sent=at;
            tx=std::move(out);completion=at+callback_delay;
        }
    }
    return c.stats();
}
// Blackouts cover a due timer (including RX and completion dispatch), rather
// than merely delaying every Nth event, which often delays only a fast callback.
void blackout_render_stream(std::chrono::milliseconds pause,unsigned depth,uint16_t pid=0x0b32,std::chrono::milliseconds tx_delay=0ms,bool uneven=false){
    const auto start=Core::Clock::time_point{}+1h;const auto p=profile(pid,true);const auto age=device_connector(pid).render_age;
    Core c(p,true);connect(c,start);const auto mic=microphone();c.radio(4,1,mic,start);
    uint32_t seq=1000;std::map<uint32_t,Core::Clock::time_point> submitted;
    auto enqueue=[&](auto at){auto r=render(seq,8,0,pid==0x0b32?2:1);for(size_t i=0;i<8;++i)for(unsigned b=0;b<4;++b)r.data[i*198+6+b]=static_cast<uint8_t>(seq>>(b*8));submitted[seq]=start+at;c.submit(std::move(r),start+at);++seq;};
    for(unsigned n=0;n<depth;++n)enqueue(0ns);
    auto tx=take(c);auto completion=100us+0ns,rx=8ms+0ns,at=0ns,last_sent=0ns;
    uint32_t last_id=1000;unsigned iterations=0,sends=1;auto max_age=0ns,max_phase=0ns,min_phase=8ms+0ns;
    std::deque<std::chrono::nanoseconds> recent_sends;
    for(;;){
        require(++iterations<200000);
        auto next=std::min(rx,completion);
        if(const auto wake=c.wake_at(start+at))next=std::min(next,std::max(at+1ns,std::chrono::duration_cast<std::chrono::nanoseconds>(*wake-start)));
        require(next>=at);at=next;if(at>=60s)break;
        // 20 separate stalls and a cluster of three; then a clean recovery tail.
        for(unsigned n=1;n<=23;++n){const auto begin=n<=20?n*1s:21s+(n-21)*16ms;const auto hold=uneven?std::chrono::milliseconds((n*17)%11):pause;if(at>=begin && at<begin+hold)at=begin+hold;}
        while(rx<=at){c.radio(4,1,mic,start+at,start+rx);rx+=8ms;}
        if(completion<=at){completion=120s;c.complete(1,tx.token,0,start+at);}
        c.tick(start+at);
        for(const auto &reply:c.replies()){(void)reply;enqueue(at);}
        const auto out=c.outbound();require(out.size()<=1);
        for(const auto &packet:out){
            require(completion==120s && at>last_sent && packet.media_deadline>start+at);
            require(packet.scheduled_at<=start+at && packet.scheduled_at>=start);
            while(!recent_sends.empty() && at-recent_sends.front()>=8ms)recent_sends.pop_front();recent_sends.push_back(at);require(recent_sends.size()<=2);
            const auto pcm=payload(packet);uint32_t id=0;for(unsigned b=0;b<4;++b)id|=uint32_t(pcm[b])<<(8*b);
            require(id>last_id);last_id=id;max_age=std::max(max_age,std::chrono::duration_cast<std::chrono::nanoseconds>(start+at-submitted.at(id)));
            submitted.erase(submitted.begin(),submitted.upper_bound(id));last_sent=at;++sends;
            if(at>50s){const auto phase=at%std::chrono::nanoseconds(8ms);max_phase=std::max(max_phase,phase);min_phase=std::min(min_phase,phase);}
            tx=packet;completion=at+100us+(at>=1s && at<22s?tx_delay:0ms);
        }
    }
    const auto &s=c.stats();
    std::cout<<"blackout_ms="<<pause.count()<<" depth="<<depth<<" pid="<<pid<<" tx_delay_ms="<<tx_delay.count()<<" uneven="<<uneven<<" expired="<<s.render_expired<<" dropped="<<s.render_dropped_frames<<" rephase="<<s.render_rephased<<" sends="<<sends<<" max_age_us="<<max_age/1us<<" recovery_phase_us="<<max_phase/1us<<'\n';
    require(c.ready() && s.render_high_water<=32 && max_age<age && s.max_render_pcm_age_us==static_cast<uint64_t>(max_age/1us));
    if(pause<=5ms && tx_delay==0ms)require(s.render_expired==0 && s.render_dropped_frames==0);
    require(max_phase-min_phase<=10us); // Stable phase after dispatch recovers.
    if(tx_delay==0ms)require(sends>=7460); // At most bounded missed slots per blackout.
    if(tx_delay==0ms)require(s.max_render_lateness_us<=10000);
}
}
int main(){try{
    const auto now=Core::Clock::time_point{}+100s;
    const auto profile2024=profile(0x0b32,true),profile2021=profile(0x0b17,true);
    const auto mic=microphone();
    const auto age2024=device_connector(0x0b32).render_age;
    {
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(900,8,1),now);auto tx=take(c);
        // The write succeeded before the worker handled its callback. It must
        // remain successful even when dispatch crosses the source-age limit.
        c.complete(1,tx.token,0,now+age2024+1ms);require(c.replies().size()==1);
        require(c.stats().render_success_after_deadline==1 && c.stats().render_expired==0 && c.stats().render_tx_expired==0 && c.stats().render_pcm_expired==0);
        c.radio(4,1,mic,now+age2024+2ms);c.submit(render(901,8,2),now+age2024+2ms);tx=take(c);
        c.complete(1,tx.token,-18,now+age2024+3ms);c.replies();
        require(c.stats().render_tx_expired==1 && c.stats().render_expired_before_tx==0 && c.stats().render_tx_failed==0);
        c.submit(render(902,8,3),now+age2024+4ms);c.tick(now+2*age2024+4ms);c.replies();
        require(c.stats().render_expired_before_tx==1 && c.stats().render_pcm_expired==8 && c.stats().render_tx_expired==1);
        c.radio(4,1,mic,now+2*age2024+5ms);c.submit(render(903,8,4),now+2*age2024+5ms);tx=take(c);
        c.complete(1,tx.token,-19,now+2*age2024+6ms);require(c.stats().render_tx_failed==1 && c.stats().recovery_required);
    }
    for(auto pid:{uint16_t(0x0b32),uint16_t(0x0b17)})for(auto depth:{8u,10u})for(auto pause:{3ms,5ms,8ms,10ms})blackout_render_stream(pause,depth,pid);
    blackout_render_stream(10ms,8,0x0b32,10ms);
    blackout_render_stream(10ms,8,0x0b32,0ms,true);
    require(age2024==88ms && device_connector(0x0b17).render_age==96ms && device_connector(0x0b12).render_age==64ms);
    const std::vector<std::vector<std::chrono::nanoseconds>> burst_patterns{{12ms,1ms,11ms},{7ms,1ms,12ms,12ms},{16ms,0ms},{8ms}};
    for(const auto &gaps:burst_patterns){
        const auto s=bursty_render_stream(gaps);
        require(s.render_expired==0 && s.render_dropped_frames==0 && s.audio_tx>=3740 && s.audio_tx<=3760);
        require(!s.recovery_required && s.generation==1 && s.render_high_water<=32);
    }
    {
        const std::array<std::chrono::nanoseconds,1> gaps{16ms};
        const auto s=bursty_render_stream(gaps,2);
        require(s.render_expired==0 && s.render_dropped_frames==0 && s.audio_tx>=3740 && s.audio_tx<=3760 && !s.recovery_required);
    }
    for(auto depth:{8u,10u})for(auto jitter:{500us,1000us,2000us,3000us,5000us,8000us,10000us})for(auto completion:{50us,100us,400us}){
        const std::array<std::chrono::nanoseconds,1> gaps{8ms};
        const auto s=bursty_render_stream(gaps,1,jitter,completion,depth);
        require(s.render_expired==0 && s.render_dropped_frames==0 && s.audio_tx>=3740 && s.audio_tx<=3760);
        require(!s.recovery_required && s.render_high_water<=32 && s.render_pending_high_water==depth);
    }
    for(auto depth:{8u,10u})for(auto actual:{7950us,8050us}){
        const std::array<std::chrono::nanoseconds,1> gaps{actual};
        const auto s=bursty_render_stream(gaps,1,0us,100us,depth);
        require(s.render_expired==0 && s.render_dropped_frames==0 && !s.recovery_required);
        const auto lead=static_cast<int64_t>(s.audio_tx)-static_cast<int64_t>(s.audio_rx);require(lead>=-3 && lead<=3);
    }
    for(auto actual:{7950us,8000us,8050us}){
        RadioRenderTiming timing;auto arrived=now;timing.observe(arrived,1);
        for(unsigned n=0;n<4096;++n){arrived+=actual;timing.observe(arrived,1);}
        const auto difference=timing.period()-actual;require(difference>=-1us && difference<=1us);
        const auto saved=timing.period();timing.observe(arrived-1ms,1);require(timing.period()==saved);
        arrived+=32ms;timing.observe(arrived,1);require(timing.period()==8ms);
        timing.clear();require(timing.period()==8ms);
    }
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
    for(auto pause:{10ms,20ms,40ms,80ms,100ms,250ms,500ms}){
        Core c(profile2024,true);connect(c,now);c.radio(4,1,mic,now);
        c.submit(render(30,8,1),now);auto first=take(c);
        c.submit(render(31,8,2),now+1ms);c.submit(render(32,8,3),now+1ms);
        const auto generation=c.stats().generation;
        // Both the callback and the new RX batch have been held by a worker stall.
        c.radio(4,1,mic,now+pause);c.complete(1,first.token,0,now+pause);c.replies();
        if(pause>=age2024){
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
        c.submit(render(41,6,2),now+age2024-1ms);require(c.outbound().empty());
        c.tick(now+age2024); // Only the old two frames have expired.
        require(c.stats().render_dropped_frames==2 && c.stats().render_expired==0);
        require(c.replies().size()==1);
        c.radio(4,1,mic,now+age2024+1ms);c.submit(render(42,2,3),now+age2024+1ms);auto tx=take(c);
        const auto pcm=payload(tx);require(pcm[0]==2 && pcm[6*192-1]==2 && pcm[6*192]==3 && pcm.back()==3);
        require(tx.media_deadline==now+2*age2024-1ms && c.stats().render_high_water<=32);
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
        c.complete(1,old.token,-18,now+age2024);c.replies();
        c.submit(render(62,4,3),now+age2024);c.replies();
        c.radio(4,1,mic,now+age2024+1ms);c.submit(render(63,4,4),now+age2024+1ms);auto fresh=take(c);
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
        c.submit(render(66,14,2),now+age2024-4ms);require(c.outbound().empty());
        c.radio(4,1,mic,now+age2024-1ms);auto mixed=take(c);require(mixed.media_deadline==now+age2024);
        c.complete(1,mixed.token,-18,now+age2024+1ms);require(c.replies().empty() && c.outbound().empty());
        c.tick(now+age2024+7ms);auto tail=take(c);const auto pcm=payload(tail);
        require(pcm.front()==2 && pcm.back()==2 && tail.media_deadline==now+2*age2024-4ms);
        c.complete(1,tail.token,0,now+age2024+7ms);auto done=c.replies();require(done.size()==1);
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
        c.tick(now+age2024);auto done=c.replies();require(done.size()==1 && c.stats().audio_tx==1);
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
    std::cout<<"PASS: burst jitter, queued render, scheduler jitter, clock drift, bounded prediction, 10/20/40/80/250/500 ms stalls, no catch-up bursts, per-frame expiry, unlink ownership and transport recovery\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

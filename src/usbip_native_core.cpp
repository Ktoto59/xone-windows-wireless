// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbip_native_core.hpp"
#include "gip_link.hpp"
#include "diagnostic_log.hpp"
#include <algorithm>
namespace xone::usbip {
namespace {
struct Packet {GipPacket decoded;Bytes wire;};
std::vector<Packet> packets(std::span<const uint8_t> data){
    if(data.empty())throw Error("Empty GIP transfer");std::vector<Packet> result;
    size_t at=0;
    auto integer=[&]{uint32_t n=0;for(unsigned i=0;i<4;++i){if(at==data.size())throw Error("Short GIP integer");const auto b=data[at++];n|=uint32_t(b&127)<<(7*i);if(!(b&128))return n;}throw Error("Oversized GIP integer");};
    while(at<data.size()){
        const auto start=at;if(data.size()-at<4)throw Error("Short GIP header");GipPacket p{data[at],data[at+1],data[at+2],0,{}};at+=3;
        const auto size=integer();if(p.options&128)p.chunk_offset=integer();if(size>data.size()-at)throw Error("Short GIP body");
        p.payload.assign(data.begin()+at,data.begin()+at+size);at+=size;result.push_back({std::move(p),{data.begin()+start,data.begin()+at}});
    }return result;
}
uint8_t sequence(uint8_t &value){const auto result=value;value=value==255?1:static_cast<uint8_t>(value+1);return result;}
Completion accepted(const Request &r){Completion c;c.frame=r.frame;for(const auto &p:r.iso){c.iso.push_back({p.offset,p.length,p.length,0});c.actual+=p.length;}return c;}
}
std::vector<Bytes> interrupt_packets(std::span<const uint8_t> data){
    std::vector<Bytes> result;
    for(auto &packet:packets(data)){
        auto &p=packet.decoded;if(p.command==0x60)throw Error("Audio on interrupt endpoint");
        if(packet.wire.size()<=64){result.push_back(std::move(packet.wire));continue;}
        const auto length=p.payload.size();
        if(!(p.options&128) || p.chunk_offset>65535 || ((p.options&64) && p.chunk_offset<length))throw Error("Invalid interrupt chunk");
        const size_t position=(p.options&64)?0:p.chunk_offset;if(position+length>65535)throw Error("Oversized logical GIP transfer");
        for(size_t at=0;at<length;at+=56){
            const auto n=std::min(size_t(56),length-at);GipPacket part{p.command,static_cast<uint8_t>(p.options&~(16|64)),p.sequence,static_cast<uint32_t>(position+at),{p.payload.begin()+at,p.payload.begin()+at+n}};
            if(!at && (p.options&64)){part.options|=64;part.chunk_offset=p.chunk_offset;}if(at+n==length)part.options|=p.options&16;
            auto wire=encode_gip(part);if(wire.size()>64)throw Error("Oversized USB subchunk");result.push_back(std::move(wire));
        }
    }return result;
}
Core::Core(const Profile &p,bool audio):profile_(p),audio_(audio),connector_(device_connector(p.pid)){}
bool Core::clock_active(Clock::time_point now)const{return connector_.independent_clock && stats_.layout && radio_last_!=Clock::time_point{} && now-radio_last_<=std::chrono::milliseconds(128);}
void Core::trim_capture(Clock::time_point now){
    const auto before=stats_.dropped;
    while(!capture_.empty() && (capture_.size()>32 || now-capture_times_.front()>std::chrono::milliseconds(32))){capture_.pop_front();capture_times_.pop_front();++stats_.dropped;}
    if(stats_.dropped!=before)diagnostic("capture_dropped",{{"wcid",stats_.wcid},{"count",stats_.dropped-before},{"total",stats_.dropped}},true);
}
void Core::clear_audio(){pcm_.clear();capture_.clear();capture_times_.clear();stats_.layout=false;credits_=0;capture_deadline_={};render_deadline_={};radio_last_={};pcm_started_={};}
void Core::reset(){
    while(!pending_.empty())fail(pending_.begin()->first,-19);
    tokens_.clear();irq_.clear();irq_wait_.clear();capture_wait_.clear();render_wait_.clear();outbound_.clear();clear_audio();
    stats_.wcid=0;++stats_.generation;capture_sequence_=render_sequence_=1;
    stats_.recovery_required=false;
}
void Core::imported(){detached();control_=std::make_unique<Control>(profile_);stats_.recovery_required=false;}
void Core::detached(){pending_.clear();tokens_.clear();irq_wait_.clear();capture_wait_.clear();render_wait_.clear();outbound_.clear();replies_.clear();control_.reset();clear_audio();}
bool Core::cancel(uint32_t seq){
    const auto it=pending_.find(seq);if(it==pending_.end())return false;
    if(it->second.token){tokens_.erase(it->second.token);std::erase_if(outbound_,[&](const auto &tx){return tx.token==it->second.token;});}
    if(it->second.kind==Kind::render && it->second.staged)pcm_.clear();
    pending_.erase(it);std::erase(irq_wait_,seq);std::erase(capture_wait_,seq);std::erase(render_wait_,seq);return true;
}
void Core::finish(uint32_t seq,Completion result){
    const auto it=pending_.find(seq);if(it==pending_.end())return;
    replies_.push_back(completion(it->second.request,result));cancel(seq);
}
void Core::fail(uint32_t seq,int32_t status){const auto it=pending_.find(seq);if(it!=pending_.end())finish(seq,failed(it->second.request,status));}
void Core::expire_render(uint32_t seq){
    const auto it=pending_.find(seq);if(it==pending_.end())return;
    ++stats_.render_expired;pcm_.clear();pcm_started_={};
    diagnostic("render_expired",{{"seq",seq},{"wcid",stats_.wcid},{"generation",stats_.generation}},true);
    Completion c;c.frame=it->second.request.frame;
    for(const auto &slot:it->second.request.iso)c.iso.push_back({slot.offset,slot.length,0,-18});
    finish(seq,std::move(c));
}
void Core::recover_audio(){
    const auto wcid=stats_.wcid;reset();stats_.wcid=wcid;stats_.recovery_required=wcid!=0;
    diagnostic("audio_recovery_required",{{"wcid",wcid},{"generation",stats_.generation}},true);
}
uint32_t Core::transmit(uint32_t seq,Bytes data,bool audio,Clock::time_point now){
    do{if(++next_token_==0)++next_token_;}while(tokens_.contains(next_token_));
    auto &p=pending_.at(seq);p.token=next_token_;p.deadline=now+std::chrono::seconds(5);tokens_[next_token_]=seq;
    outbound_.push_back({stats_.wcid,next_token_,std::move(data),audio,audio?p.media_deadline:Clock::time_point::max()});return next_token_;
}
void Core::submit(Request r,Clock::time_point now){
    if(diagnostic_verbose)diagnostic("urb_submit",{{"seq",r.seq},{"ep",r.ep},{"direction",r.direction},{"length",r.length},{"pending",pending_.size()}});
    if(!control_)throw Error("URB before USB import");if(pending_.contains(r.seq))throw Error("Duplicate active URB sequence");
    if(r.command==2){const bool cancelled=cancel(r.unlink_target);replies_.push_back(unlinked(r.seq,cancelled));pump(now);return;}
    auto reject=[&](int32_t status){replies_.push_back(completion(r,failed(r,status)));};
    if(!r.ep){
        try{
            auto data=control_->submit(r);Completion c;c.actual=static_cast<uint32_t>(data.size());c.data=std::move(data);replies_.push_back(completion(r,c));
            if((r.setup[0]==0 && r.setup[1]==9) || (r.setup[0]==1 && r.setup[1]==11)){
                std::vector<uint32_t> disabled;
                for(const auto &[seq,p]:pending_)if(r.setup[1]==9 || !control_->endpoint(static_cast<uint8_t>(p.request.ep|(p.request.direction?128:0))))disabled.push_back(seq);
                for(auto seq:disabled)fail(seq,-108);
                if(r.setup[1]==9 || (r.setup[2]==0 && r.setup[3]==0 && r.setup[4]==1 && r.setup[5]==0))clear_audio();
            }
        }catch(const Stall &){reject(-32);}pump(now);return;
    }
    const auto *endpoint=control_->endpoint(static_cast<uint8_t>(r.ep|(r.direction?128:0)));
    if(!endpoint){reject(-32);return;}if(bool(r.iso.size())!=(endpoint->type==1)){reject(-22);return;}
    if(pending_.size()>=64){reject(-12);return;}if(!ready()){reject(-19);return;}
    const auto seq=r.seq;const bool iso=endpoint->type==1;
    if(iso && (!audio_ || r.iso.size()>32)){reject(-95);return;}
    const auto kind=iso?(r.direction?Kind::capture:Kind::render):(r.direction?Kind::irq_in:Kind::irq_out);
    if(!iso && !r.direction){
        try{auto parsed=packets(r.data);if(r.length>64 || std::any_of(parsed.begin(),parsed.end(),[](const auto &p){return p.decoded.command==0x60;})){reject(-95);return;}
            for(const auto &p:parsed)if(p.decoded.command==5){++stats_.power_commands;stats_.last_power=hex(p.wire);}}
        catch(const Error &){reject(-71);return;}
    }
    if(kind==Kind::irq_out && connector_.preserve_wireless){
        const Bytes zero(14),observed{0,0,0,0,0,0,0x55,0x53,0,0,0,0,0,0};Bytes forwarded;
        for(const auto &packet:packets(r.data)){
            const auto &g=packet.decoded;const bool consume=g.command==5 && g.options==32 && g.payload.size()==15 && g.payload[0]==6 &&
                (std::equal(g.payload.begin()+1,g.payload.end(),zero.begin()) || std::equal(g.payload.begin()+1,g.payload.end(),observed.begin()));
            if(consume)++stats_.mode_suppressed;else forwarded.insert(forwarded.end(),packet.wire.begin(),packet.wire.end());
        }
        if(forwarded.empty()){Completion c;c.actual=static_cast<uint32_t>(r.length);replies_.push_back(completion(r,c));return;}
        // Retain the USB transfer's original length; only the consumed command
        // is removed from the RF payload below.
        Pending p{std::move(r),kind,false,0,now+std::chrono::seconds(5),now+connector_.render_age};pending_.emplace(seq,std::move(p));transmit(seq,std::move(forwarded),false,now);return;
    }
    Pending p{std::move(r),kind,false,0,now+std::chrono::seconds(5),now+connector_.render_age};pending_.emplace(seq,std::move(p));
    switch(kind){
    case Kind::irq_in:irq_wait_.push_back(seq);break;
    case Kind::capture:capture_wait_.push_back(seq);break;
    case Kind::render:render_wait_.push_back(seq);break;
    case Kind::irq_out:transmit(seq,pending_.at(seq).request.data,false,now);break;
    }pump(now);
}
void Core::pump(Clock::time_point now){
    trim_capture(now);
    auto head=[&](std::deque<uint32_t> &q)->std::optional<uint32_t>{while(!q.empty() && !pending_.contains(q.front()))q.pop_front();return q.empty()?std::optional<uint32_t>{}:q.front();};
    while(auto seq=head(irq_wait_)){
        if(irq_.empty())break;const auto &r=pending_.at(*seq).request;
        if(irq_.front().size()>static_cast<size_t>(r.length)){fail(*seq,-90);continue;}
        Completion c;c.data=std::move(irq_.front());irq_.pop_front();c.actual=static_cast<uint32_t>(c.data.size());finish(*seq,std::move(c));
    }
    while(auto seq=head(capture_wait_)){
        const auto &r=pending_.at(*seq).request;const auto count=r.iso.size();if(capture_.size()<count && !clock_active(now))break;
        if(std::any_of(r.iso.begin(),r.iso.end(),[](const Iso &slot){return slot.length<54;})){fail(*seq,-90);continue;}
        if(connector_.independent_clock && capture_deadline_==Clock::time_point{})capture_deadline_=now+std::chrono::milliseconds(8);
        if(now<capture_deadline_)break;
        const auto duration=std::chrono::milliseconds(count);capture_deadline_=std::max(capture_deadline_+duration,now-duration);
        Completion c;c.frame=r.frame;
        for(const auto &slot:r.iso){Bytes frame;if(!capture_.empty()){frame=std::move(capture_.front());capture_.pop_front();capture_times_.pop_front();}else{frame={0x60,32,sequence(capture_sequence_),50,192,0};frame.resize(54);++stats_.concealed;}c.iso.push_back({slot.offset,slot.length,static_cast<uint32_t>(frame.size()),0});c.data.insert(c.data.end(),frame.begin(),frame.end());}
        c.actual=static_cast<uint32_t>(c.data.size());stats_.capture_slots+=count;finish(*seq,std::move(c));
    }
    while(auto seq=head(render_wait_)){
        auto &p=pending_.at(*seq);const auto &r=p.request;if(p.token)break;
        if(now>p.media_deadline){expire_render(*seq);continue;}
        if(pcm_started_!=Clock::time_point{} && now-pcm_started_>connector_.render_age){
            if(p.staged){expire_render(*seq);continue;}
            pcm_.clear();pcm_started_={};
        }
        if(!p.staged){
            if(!stats_.layout){fail(*seq,-95);continue;}Bytes incoming;
            try{for(const auto &slot:r.iso)for(const auto &frame:packets(std::span<const uint8_t>(r.data).subspan(slot.offset,slot.length))){
                const auto &g=frame.decoded;if(g.command!=0x60 || g.options!=32 || g.payload.size()!=192)throw Stall("Unsupported render PCM");incoming.insert(incoming.end(),g.payload.begin(),g.payload.end());
            }}catch(const Stall &){fail(*seq,-95);continue;}catch(const Error &){fail(*seq,-71);continue;}
            if(pcm_.size()+incoming.size()>192*32){fail(*seq,-12);continue;}
            if(pcm_.empty())pcm_started_=now;
            pcm_.insert(pcm_.end(),incoming.begin(),incoming.end());p.staged=true;
            p.media_deadline=std::min(p.media_deadline,pcm_started_+connector_.render_age);
        }
        if(pcm_.size()<1536){p.staged=false;finish(*seq,accepted(r));continue;}
        if(connector_.independent_clock){if(!clock_active(now) || now<render_deadline_)break;render_deadline_=render_deadline_==Clock::time_point{}?now+std::chrono::milliseconds(8):std::max(render_deadline_+std::chrono::milliseconds(8),now);}
        else {if(now-radio_last_>std::chrono::milliseconds(32))credits_=0;if(!credits_)break;--credits_;}
        Bytes wire{0x60,32,sequence(render_sequence_),0x80,0x8c,0};wire.insert(wire.end(),pcm_.begin(),pcm_.begin()+1536);pcm_.erase(pcm_.begin(),pcm_.begin()+1536);transmit(*seq,std::move(wire),true,now);break;
    }
}
void Core::radio(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data,Clock::time_point now){
    if(kind==1){if(!wcid || wcid>16 || data.size()!=6)throw Error("Invalid association");reset();stats_.wcid=wcid;return;}
    if(!wcid || wcid!=stats_.wcid)return;
    if(kind==3){reset();return;}
    if(kind==2){auto wire=interrupt_packets(data);if(irq_.size()+wire.size()>256)throw Error("Interrupt queue overflow");stats_.rx+=packets(data).size();for(auto &p:wire)irq_.push_back(std::move(p));}
    else if(kind==4){
        auto parsed=packets(data);stats_.audio_rx+=parsed.size();
        if(std::any_of(parsed.begin(),parsed.end(),[](const auto &p){return p.decoded.command!=0x60 || p.decoded.payload.size()!=386 || p.decoded.payload[0]!=0 || p.decoded.payload[1]!=6;}))throw Error("Unsupported radio PCM layout");
        stats_.layout=true;radio_last_=now;if(!audio_)return;
        if(parsed.size()>8){stats_.dropped+=(parsed.size()-8)*8;parsed.erase(parsed.begin(),parsed.end()-8);}
        const auto count=parsed.size()*8;
        (void)count;
        for(const auto &p:parsed)for(size_t at=2;at<386;at+=48){Bytes frame{0x60,32,sequence(capture_sequence_),50,192,0};frame.insert(frame.end(),p.decoded.payload.begin()+at,p.decoded.payload.begin()+at+48);capture_.push_back(std::move(frame));capture_times_.push_back(now);}trim_capture(now);
        stats_.capture_high_water=std::max(stats_.capture_high_water,capture_.size());credits_=std::min(size_t(2),credits_+parsed.size());
    }else throw Error("Unknown radio event");pump(now);
}
void Core::complete(uint8_t wcid,uint32_t token,int32_t status,Clock::time_point now){
    if(status || diagnostic_verbose)diagnostic("radio_tx_complete",{{"wcid",wcid},{"token",token},{"status",status}},status!=0);
    if(wcid!=stats_.wcid)return;const auto t=tokens_.find(token);if(t==tokens_.end())return;
    const auto seq=t->second;tokens_.erase(t);auto it=pending_.find(seq);if(it==pending_.end())return;auto &p=it->second;p.token=0;
    // A deliberately expired, unsent PCM block is media loss, not RF failure.
    if(status==-18 && p.kind==Kind::render){expire_render(seq);pump(now);return;}
    if(status){const bool audio=p.kind==Kind::render;fail(seq,status);if(audio)recover_audio();pump(now);return;}
    if(p.kind==Kind::render){++stats_.audio_tx;p.deadline=now+std::chrono::seconds(5);}
    else {stats_.tx+=packets(p.request.data).size();Completion c;c.actual=static_cast<uint32_t>(p.request.length);finish(seq,c);}
    pump(now);
}
void Core::tick(Clock::time_point now){
    std::vector<uint32_t> expired;for(const auto &[seq,p]:pending_)if((p.kind==Kind::irq_out || (p.kind==Kind::render && p.token)) && now>=p.deadline)expired.push_back(seq);
    bool audio_timeout=false;
    for(auto seq:expired){const auto p=pending_.find(seq);if(p!=pending_.end())audio_timeout|=p->second.kind==Kind::render;fail(seq,-19);}
    if(audio_timeout)recover_audio();pump(now);
}
std::optional<Core::Clock::time_point> Core::wake_at()const{
    std::optional<Clock::time_point> result;
    for(const auto &[seq,p]:pending_){(void)seq;if(p.kind==Kind::irq_out || p.kind==Kind::render)if(!result || p.deadline<*result)result=p.deadline;}
    auto need=[&](Clock::time_point at){if(!result || at<*result)result=at;};
    if(!capture_wait_.empty() && pending_.contains(capture_wait_.front())){const auto &r=pending_.at(capture_wait_.front()).request;if(capture_.size()>=r.iso.size() || clock_active(Clock::now()))need(capture_deadline_);}
    if(!render_wait_.empty() && pending_.contains(render_wait_.front())){const auto &p=pending_.at(render_wait_.front());if(!p.token){need(p.media_deadline);if(connector_.independent_clock && clock_active(Clock::now()) && pcm_.size()>=1536)need(render_deadline_);}}
    return result;
}
}

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
Core::Core(const Profile &p,bool audio):profile_(p),audio_(audio),connector_(device_connector(p.pid)){pcm_.reserve(32);}
bool Core::clock_active(Clock::time_point now)const{return connector_.independent_clock && stats_.layout && radio_last_!=Clock::time_point{} && now-radio_last_<=std::chrono::milliseconds(128);}
bool Core::render_clock_active(Clock::time_point now)const{
    if(connector_.independent_clock)return clock_active(now);
    // RX audio is a local pacing hint, not a protocol grant. Bridge short gaps
    // only after one real render send; stop prediction at the existing 32 ms
    // radio freshness limit. No audio is generated while the worker is stalled.
    return stats_.layout && render_deadline_!=Clock::time_point{} &&
        radio_last_!=Clock::time_point{} && now-radio_last_<std::chrono::milliseconds(32);
}
void Core::trim_render(Clock::time_point now){
    const auto before=pcm_.size();
    std::erase_if(pcm_,[&](const auto &frame){return now>=frame.expires;});
    stats_.render_dropped_frames+=before-pcm_.size();
}
void Core::trim_capture(Clock::time_point now){
    while(!capture_.empty() && (capture_.size()>32 || now-capture_times_.front()>std::chrono::milliseconds(32))){capture_.pop_front();capture_times_.pop_front();++stats_.dropped;}
    report_media_drops(now);
}
void Core::report_media_drops(Clock::time_point now){
    if(stats_.dropped!=reported_capture_drops_ && (capture_reported_at_==Clock::time_point{} || now-capture_reported_at_>=std::chrono::seconds(1))){
        diagnostic("capture_dropped",{{"wcid",stats_.wcid},{"count",stats_.dropped-reported_capture_drops_},{"total",stats_.dropped},{"pending_capture",capture_wait_.size()}},true);
        reported_capture_drops_=stats_.dropped;capture_reported_at_=now;++stats_.capture_drop_reports;
    }
    if(stats_.render_expired!=reported_render_expired_ && (render_reported_at_==Clock::time_point{} || now-render_reported_at_>=std::chrono::seconds(1))){
        diagnostic("render_expired",{{"wcid",stats_.wcid},{"generation",stats_.generation},{"count",stats_.render_expired-reported_render_expired_},{"total",stats_.render_expired}},true);
        reported_render_expired_=stats_.render_expired;render_reported_at_=now;++stats_.render_expiry_reports;
    }
}
void Core::clear_audio(){pcm_.clear();capture_.clear();capture_times_.clear();stats_.layout=false;credits_=0;capture_deadline_={};render_deadline_={};radio_last_={};render_timing_.clear();bad_audio_streak_=0;}
void Core::reset(){
    while(!pending_.empty())fail(pending_.begin()->first,-19);
    tokens_.clear();irq_.clear();irq_wait_.clear();capture_wait_.clear();render_wait_.clear();outbound_.clear();clear_audio();
    stats_.wcid=0;++stats_.generation;capture_sequence_=render_sequence_=1;
    last_audio_resync_.reset();
    announce_.clear();imported_once_=false;
    initialization_={};
    stats_.recovery_required=false;
}
void Core::imported(){
    detached();
    // Only the immutable identity announcement belongs in a new USB session.
    // Never replay previous acknowledgements, power or audio commands.
    if(imported_once_){irq_.clear();if(!announce_.empty()){irq_.push_back(announce_);++stats_.announce_replays;}}
    imported_once_=true;++stats_.usb_imports;
    initialization_={};
    control_=std::make_unique<Control>(profile_);stats_.recovery_required=false;
    diagnostic("usb_session_initialized",{{"wcid",stats_.wcid},{"imports",stats_.usb_imports},{"announce_replays",stats_.announce_replays}});
}
void Core::detached(){for(auto &[seq,p]:pending_){(void)seq;p.cancellation.request_stop();}pending_.clear();tokens_.clear();irq_wait_.clear();capture_wait_.clear();render_wait_.clear();outbound_.clear();replies_.clear();control_.reset();clear_audio();}
bool Core::cancel(uint32_t seq){
    const auto it=pending_.find(seq);if(it==pending_.end())return false;
    it->second.cancellation.request_stop();
    if(it->second.token){tokens_.erase(it->second.token);std::erase_if(outbound_,[&](const auto &tx){return tx.token==it->second.token;});}
    if(it->second.kind==Kind::render && it->second.staged)
        std::erase_if(pcm_,[&](const auto &frame){return frame.owner==seq;});
    pending_.erase(it);std::erase(irq_wait_,seq);std::erase(capture_wait_,seq);std::erase(render_wait_,seq);return true;
}
void Core::finish(uint32_t seq,Completion result){
    const auto it=pending_.find(seq);if(it==pending_.end())return;
    replies_.push_back(completion(it->second.request,result));cancel(seq);
}
void Core::fail(uint32_t seq,int32_t status){const auto it=pending_.find(seq);if(it!=pending_.end())finish(seq,failed(it->second.request,status));}
void Core::expire_render(uint32_t seq,Clock::time_point now){
    const auto it=pending_.find(seq);if(it==pending_.end())return;
    auto result=render_result(it->second,true);
    if(!it->second.media_lost && std::any_of(result.iso.begin(),result.iso.end(),[](const auto &slot){return slot.status!=0;}))++stats_.render_expired;
    trim_render(now);
    report_media_drops(now);
    finish(seq,std::move(result));
}
Completion Core::render_result(const Pending &p,bool expired)const{
    auto result=accepted(p.request);
    for(size_t i=0;i<result.iso.size();++i){
        const bool sent=p.slot_frames[i] && p.sent_frames[i]==p.slot_frames[i];
        if((p.lost_slots&(uint32_t(1)<<i)) || (expired && !sent)){
            result.actual-=result.iso[i].actual;result.iso[i].actual=0;result.iso[i].status=-18;
        }
    }
    return result;
}
void Core::bad_audio(Clock::time_point now){
    ++stats_.malformed_audio;
    if(++bad_audio_streak_<8)return;
    // One corrupt/unsupported media frame is not a broken RF association.
    // Sustained bad layout requests USB/audio reinitialization only, at most
    // once per ten seconds. Preserve this cooldown across recover_audio/reset.
    if(last_audio_resync_ && now-*last_audio_resync_<std::chrono::seconds(10))return;
    ++stats_.audio_resyncs;recover_audio();last_audio_resync_=now;
}
void Core::recover_audio(){
    const auto wcid=stats_.wcid;auto announce=announce_;const auto was_imported=imported_once_;
    reset();announce_=std::move(announce);imported_once_=was_imported;stats_.wcid=wcid;stats_.recovery_required=wcid!=0;
    diagnostic("audio_recovery_required",{{"wcid",wcid},{"generation",stats_.generation}},true);
}
uint32_t Core::transmit(uint32_t seq,Bytes data,bool audio,Clock::time_point now){
    do{if(++next_token_==0)++next_token_;}while(tokens_.contains(next_token_));
    auto &p=pending_.at(seq);p.token=next_token_;p.deadline=now+std::chrono::seconds(5);tokens_[next_token_]=seq;
    p.cancellation=std::stop_source{};
    outbound_.push_back({stats_.wcid,next_token_,std::move(data),audio,audio?p.media_deadline:Clock::time_point::max(),p.cancellation.get_token()});return next_token_;
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
                auto &count=r.setup[1]==9?initialization_.configurations:initialization_.interfaces;
                if(++count==1)diagnostic("usb_initialization_control",{{"wcid",stats_.wcid},{"import",stats_.usb_imports},{"request",r.setup[1]},{"value",r.setup[2]|(unsigned(r.setup[3])<<8)},{"index",r.setup[4]|(unsigned(r.setup[5])<<8)}});
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
            for(const auto &p:parsed){
                if(p.decoded.command==5){++stats_.power_commands;stats_.last_power=hex(p.wire);}
                if(p.decoded.command<64){const auto bit=uint64_t(1)<<p.decoded.command;
                    if(!(initialization_.commands&bit))diagnostic("usb_initialization_gip",{{"wcid",stats_.wcid},{"import",stats_.usb_imports},{"command",p.decoded.command},{"length",p.wire.size()}});
                    initialization_.commands|=bit;
                }
            }}
        catch(const Error &){reject(-71);return;}
    }
    auto &requests=kind==Kind::irq_in?initialization_.irq_in:kind==Kind::irq_out?initialization_.irq_out:kind==Kind::capture?initialization_.capture:initialization_.render;
    if(++requests==1)diagnostic("usb_initialization_endpoint",{{"wcid",stats_.wcid},{"import",stats_.usb_imports},{"endpoint",r.ep},{"direction",r.direction},{"iso",iso}});
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
    trim_render(now);
    auto head=[&](std::deque<uint32_t> &q)->std::optional<uint32_t>{while(!q.empty() && !pending_.contains(q.front()))q.pop_front();return q.empty()?std::optional<uint32_t>{}:q.front();};
    while(auto seq=head(irq_wait_)){
        if(irq_.empty())break;const auto &r=pending_.at(*seq).request;
        if(irq_.front().size()>static_cast<size_t>(r.length)){fail(*seq,-90);continue;}
        if(++initialization_.irq_delivered==1)diagnostic("usb_initialization_reply",{{"wcid",stats_.wcid},{"import",stats_.usb_imports},{"command",irq_.front().front()},{"length",irq_.front().size()}});
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
        if(now>=p.media_deadline){expire_render(*seq,now);continue;}
        if(!p.staged){
            if(!stats_.layout){fail(*seq,-95);continue;}Bytes incoming;std::vector<size_t> slots;
            incoming.reserve(r.iso.size()*192);slots.reserve(r.iso.size());
            try{for(size_t i=0;i<r.iso.size();++i)for(const auto &frame:packets(std::span<const uint8_t>(r.data).subspan(r.iso[i].offset,r.iso[i].length))){
                const auto &g=frame.decoded;if(g.command!=0x60 || g.options!=32 || g.payload.size()!=192)throw Stall("Unsupported render PCM");incoming.insert(incoming.end(),g.payload.begin(),g.payload.end());slots.push_back(i);
            }}catch(const Stall &){fail(*seq,-95);continue;}catch(const Error &){fail(*seq,-71);continue;}
            if(pcm_.size()+incoming.size()/192>32){fail(*seq,-12);continue;}
            for(size_t at=0;at<incoming.size();at+=192){
                RenderFrame frame{{},p.media_deadline,*seq,slots[at/192]};
                std::copy_n(incoming.begin()+at,192,frame.data.begin());pcm_.push_back(std::move(frame));
                ++p.slot_frames[slots[at/192]];
            }
            stats_.render_high_water=std::max(stats_.render_high_water,pcm_.size());p.staged=true;
        }
        if(pcm_.size()<8){
            // These short fragments are accepted into the bounded aggregate.
            // Disown them before USB/IP can reuse this completed sequence ID.
            for(auto &frame:pcm_)if(frame.owner==*seq)frame.owner.reset();
            auto result=render_result(p,false);
            p.staged=false;finish(*seq,std::move(result));continue;
        }
        if(now<render_deadline_)break;
        if(connector_.independent_clock){if(!render_clock_active(now))break;}
        else {
            if(now-radio_last_>=std::chrono::milliseconds(32))credits_=0;
            if(!credits_ && !render_clock_active(now))break;
            if(credits_)--credits_;else ++stats_.render_predicted;
        }
        // Re-anchor after a stall, rather than replaying missed timer slots in
        // a burst when asynchronous completions arrive at the same timestamp.
        const auto period=connector_.independent_clock?std::chrono::nanoseconds(std::chrono::milliseconds(8)):render_timing_.period();
        if(render_deadline_==Clock::time_point{})render_deadline_=now+period;
        else if(now-render_deadline_>=period){++stats_.render_rephased;render_deadline_=now+period;}
        else render_deadline_+=period;
        Bytes wire{0x60,32,sequence(render_sequence_),0x80,0x8c,0};
        auto media_deadline=p.media_deadline;
        p.sending_slots=0;p.sending_frames.fill(0);
        for(unsigned i=0;i<8;++i){const auto &frame=pcm_[i];if(frame.owner==*seq){p.sending_slots|=uint32_t(1)<<frame.slot;++p.sending_frames[frame.slot];}media_deadline=std::min(media_deadline,frame.expires);wire.insert(wire.end(),frame.data.begin(),frame.data.end());}
        pcm_.erase(pcm_.begin(),pcm_.begin()+8);
        transmit(*seq,std::move(wire),true,now);
        // The oldest constituent frame bounds this physical send only. Keep
        // the request's own deadline for its remaining, possibly fresher PCM.
        outbound_.back().media_deadline=media_deadline;break;
    }
}
void Core::radio(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data,Clock::time_point now,std::optional<Clock::time_point> arrived){
    if(kind==1){if(!wcid || wcid>16 || data.size()!=6)throw Error("Invalid association");reset();stats_.wcid=wcid;return;}
    if(!wcid || wcid!=stats_.wcid)return;
    if(kind==3){reset();return;}
    if(kind==2){
        const auto decoded=packets(data);
        for(const auto &p:decoded)if(p.decoded.command==2 && p.decoded.options==32 && p.decoded.payload.size()>=28 && p.wire.size()<=64){
            const auto &body=p.decoded.payload;
            const auto vendor=body[8]|(unsigned(body[9])<<8),product=body[10]|(unsigned(body[11])<<8);
            if(vendor==0x045e && (product==profile_.pid || (profile_.pid==0x0b17 && product==0x0b31) || (profile_.pid==0x0b12 && product==0x0b13)))announce_=p.wire;
        }
        auto wire=interrupt_packets(data);if(irq_.size()+wire.size()>256)throw Error("Interrupt queue overflow");stats_.rx+=decoded.size();for(auto &p:wire)irq_.push_back(std::move(p));
    }
    else if(kind==4){
        const auto received=std::min(arrived.value_or(now),now);
        if(now-received>=std::chrono::milliseconds(32) || (radio_last_!=Clock::time_point{} && received<radio_last_)){++stats_.stale_audio;pump(now);return;}
        std::vector<Packet> parsed;
        try{parsed=packets(data);}catch(const Error &){bad_audio(now);pump(now);return;}
        stats_.audio_rx+=parsed.size();
        std::erase_if(parsed,[&](const auto &p){
            const auto &g=p.decoded;
            if(g.command==0x60 && g.payload.size()==386 && g.payload[0]==0 && g.payload[1]==6){bad_audio_streak_=0;return false;}
            bad_audio(now);return true;
        });
        if(parsed.empty()){pump(now);return;}
        if(radio_last_!=Clock::time_point{} && received>radio_last_)stats_.max_radio_gap_us=std::max(stats_.max_radio_gap_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(received-radio_last_).count()));
        if(!connector_.independent_clock)render_deadline_+=render_timing_.observe(received,parsed.size(),render_deadline_);
        stats_.layout=true;radio_last_=received;if(!audio_)return;
        if(!control_){stats_.capture_without_usb+=parsed.size()*8;return;}
        if(parsed.size()>8){stats_.dropped+=(parsed.size()-8)*8;parsed.erase(parsed.begin(),parsed.end()-8);}
        const auto count=parsed.size()*8;
        (void)count;
        for(const auto &p:parsed)for(size_t at=2;at<386;at+=48){Bytes frame{0x60,32,sequence(capture_sequence_),50,192,0};frame.insert(frame.end(),p.decoded.payload.begin()+at,p.decoded.payload.begin()+at+48);capture_.push_back(std::move(frame));capture_times_.push_back(received);}trim_capture(now);
        stats_.capture_high_water=std::max(stats_.capture_high_water,capture_.size());credits_=std::min(size_t(2),credits_+parsed.size());
    }else throw Error("Unknown radio event");pump(now);
}
void Core::complete(uint8_t wcid,uint32_t token,int32_t status,Clock::time_point now){
    if((status && status!=-18) || diagnostic_verbose)diagnostic("radio_tx_complete",{{"wcid",wcid},{"token",token},{"status",status}},status!=0);
    if(wcid!=stats_.wcid)return;const auto t=tokens_.find(token);if(t==tokens_.end())return;
    const auto seq=t->second;tokens_.erase(t);auto it=pending_.find(seq);if(it==pending_.end())return;auto &p=it->second;p.token=0;
    // A deliberately expired, unsent PCM block is media loss, not RF failure.
    if(status==-18 && p.kind==Kind::render){
        if(!p.media_lost){p.media_lost=true;++stats_.render_expired;}
        p.lost_slots|=p.sending_slots;
        p.sending_slots=0;p.sending_frames.fill(0);stats_.render_dropped_frames+=8;report_media_drops(now);
        // A mixed block can expire because of older, already accepted PCM.
        // Continue its still-fresh tail and report loss only for affected ISO
        // slots when this USB request finishes. No RF reset or stale retry.
        if(now>=p.media_deadline)expire_render(seq,now);
        pump(now);return;
    }
    if(status){const bool audio=p.kind==Kind::render;fail(seq,status);if(audio)recover_audio();pump(now);return;}
    if(p.kind==Kind::render){for(size_t i=0;i<p.sent_frames.size();++i)p.sent_frames[i]+=p.sending_frames[i];p.sending_frames.fill(0);p.sending_slots=0;++stats_.audio_tx;p.deadline=now+std::chrono::seconds(5);}
    else {stats_.tx+=packets(p.request.data).size();Completion c;c.actual=static_cast<uint32_t>(p.request.length);finish(seq,c);}
    pump(now);
}
void Core::tick(Clock::time_point now){
    std::vector<uint32_t> expired;for(const auto &[seq,p]:pending_)if((p.kind==Kind::irq_out || (p.kind==Kind::render && p.token)) && now>=p.deadline)expired.push_back(seq);
    bool audio_timeout=false;
    for(auto seq:expired){const auto p=pending_.find(seq);if(p!=pending_.end())audio_timeout|=p->second.kind==Kind::render;fail(seq,-19);}
    if(audio_timeout)recover_audio();pump(now);
}
std::optional<Core::Clock::time_point> Core::wake_at(Clock::time_point now)const{
    std::optional<Clock::time_point> result;
    for(const auto &[seq,p]:pending_){(void)seq;if(p.kind==Kind::irq_out || p.kind==Kind::render)if(!result || p.deadline<*result)result=p.deadline;}
    auto need=[&](Clock::time_point at){if(!result || at<*result)result=at;};
    if(!capture_wait_.empty() && pending_.contains(capture_wait_.front())){const auto &r=pending_.at(capture_wait_.front()).request;if(capture_.size()>=r.iso.size() || clock_active(now))need(capture_deadline_);}
    if(!render_wait_.empty() && pending_.contains(render_wait_.front())){const auto &p=pending_.at(render_wait_.front());if(!p.token){
        need(p.media_deadline);
        if(pcm_.size()>=8 && (render_clock_active(now) || (!connector_.independent_clock && credits_ && now-radio_last_<std::chrono::milliseconds(32))))need(render_deadline_);
    }}
    return result;
}
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Port of xone transport/dongle.c, Copyright (C) 2021 Severin von Wnuck-Lipinski.
#include "session.hpp"
#include "headset_audio.hpp"
#include "diagnostic_log.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
namespace xone {
Bytes wlan_packet(uint8_t wcid,std::span<const uint8_t> client,std::span<const uint8_t> host,bool encrypted,std::span<const uint8_t> gip,bool audio) {
    if (!wcid || wcid>16 || client.size()!=6 || host.size()!=6 || gip.size()>1600) throw std::runtime_error("Invalid WLAN transmit arguments");
    const uint8_t index=static_cast<uint8_t>(wcid-1);
    const size_t head=8+sizeof(mt76_txwi)+26+2;
    auto *skb=xone_mt76_alloc_message(static_cast<int>(head+gip.size()),GFP_KERNEL);
    if (!skb) throw std::bad_alloc();
    // QoS 802.11 header has two alignment bytes after it (not included in TXWI length).
    std::array<uint8_t,26> hdr{0x88,static_cast<uint8_t>(2|(encrypted?64:0)),144,0};
    std::copy(client.begin(),client.end(),hdr.begin()+4); std::copy(host.begin(),host.end(),hdr.begin()+10); std::copy(host.begin(),host.end(),hdr.begin()+16);
    mt76_txwi tx{};
    tx.flags=static_cast<uint16_t>(FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY,IEEE80211_HT_MPDU_DENSITY_4));
    tx.rate=static_cast<uint16_t>(FIELD_PREP(MT_RXWI_RATE_PHY,MT_PHY_TYPE_OFDM)); tx.ack_ctl=MT_TXWI_ACK_CTL_REQ; tx.wcid=index; tx.len_ctl=static_cast<uint16_t>(26+gip.size());
    const std::array<uint8_t,8> prefix{0,0,static_cast<uint8_t>(audio?2:0),index,0,0,0,0}; const std::array<uint8_t,2> pad{};
    skb_put_data(skb,prefix.data(),prefix.size()); skb_put_data(skb,&tx,sizeof(tx)); skb_put_data(skb,hdr.data(),hdr.size()); skb_put_data(skb,pad.data(),pad.size()); skb_put_data(skb,gip.data(),gip.size());
    xone_mt76_prep_command(skb,static_cast<mt76_mcu_cmd>(0)); Bytes result(skb->data,skb->data+skb->len); consume_skb(skb); return result;
}
std::vector<std::string> identification_classes(std::span<const uint8_t> data) {
    if (data.size()<32) throw std::runtime_error("Truncated identification");
    auto word=[&](size_t off) -> size_t { if(off>data.size() || data.size()-off<2) throw std::runtime_error("Invalid identification offset"); return data[off]|(size_t(data[off+1])<<8); };
    size_t offset=16+word(26);
    if(offset>=data.size()) throw std::runtime_error("Classes outside identification");
    const auto count=data[offset++]; if(!count) throw std::runtime_error("Empty identification classes");
    std::vector<std::string> out;
    for(unsigned i=0;i<count;++i) {
        const auto n=word(offset); offset+=2;
        if(!n || n>data.size()-offset) throw std::runtime_error("Invalid class length");
        size_t text_length=n; while(text_length && !data[offset+text_length-1]) --text_length;
        if(!text_length || std::find(data.begin()+offset,data.begin()+offset+text_length,0)!=data.begin()+offset+text_length)
            throw std::runtime_error("Invalid class string");
        out.emplace_back(reinterpret_cast<const char *>(data.data()+offset),text_length); offset+=n;
    }
    return out;
}
struct Session::Client {
    Session &owner; uint8_t wcid; Bytes address; bool encrypted=false;
    GamepadInput input{};bool guide=false,rumble_pending=false;
    GamepadCapabilities capabilities{};
    std::optional<BatteryStatus> battery;
    bool status_disconnected=false;
    std::string output_error;
    unsigned output_recovery_attempt=0;
    std::chrono::steady_clock::time_point output_retry{},created=std::chrono::steady_clock::now();
    unsigned identify_attempts=0;
    std::chrono::steady_clock::time_point identify_retry=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    bool gamepad=false;
    std::string kind="unknown";
    std::array<uint8_t,9> pending_rumble{}; std::chrono::steady_clock::time_point rumble_last{};
    void publish(){if(gamepad && encrypted && owner.output_)owner.output_->update(wcid,input,guide);}
    void rumble(uint8_t l,uint8_t s) {rumble(rumble_packet(l,s));}
    void rumble(const std::array<uint8_t,9> &bytes) {
        if(!gamepad || !encrypted || !logical[0])return;
        logical[0]->link.send(9,bytes,false,false);
        ++owner.rumbles_;rumble_last=std::chrono::steady_clock::now();rumble_pending=false;
        std::cout<<"RUMBLE WCID="<<unsigned(wcid)<<" large="<<unsigned(bytes[4])<<" small="<<unsigned(bytes[5])<<" LT="<<unsigned(bytes[2])<<" RT="<<unsigned(bytes[3])<<" percent\n";
    }
    struct Logical {
        Client &radio; uint8_t id; bool identified=false;
        uint16_t vendor=0,product=0;
        bool pc_auth_opt_out=false;
        GipLink link; Auth auth;
        std::string auth_stage="not-started";
        bool auth_failed=false;
        std::chrono::steady_clock::time_point auth_deadline{};
        std::unique_ptr<HeadsetAudio> audio;
        uint8_t audio_sequence=1;
        uint64_t audio_received=0,audio_silence=0;
        uint64_t audio_playback=0,audio_capture=0;
        bool audio_connected=false;
        std::chrono::steady_clock::time_point audio_last{};
        std::chrono::steady_clock::time_point audio_sent_at{};
        bool audio_allowed() const{return radio.encrypted || pc_auth_opt_out;}
        uint64_t audio_tx=0,audio_late=0,audio_resets=0;
        int64_t audio_interval_min=INT64_MAX,audio_interval_max=0,audio_write_max=0;
        Logical(Client &c,uint8_t client_id):radio(c),id(client_id),link(id,[this](const GipPacket &p){radio.transmit(p);},[this](const GipPacket &p){dispatch(p);}),
            auth([this](std::span<const uint8_t> b,bool ack){link.send(6,b,ack);},[this](std::span<const uint8_t> key){
                if(id) throw std::runtime_error("Encryption requested by accessory");
                radio.owner.check(xone_mt76_set_client_key(&radio.owner.mt_,radio.wcid,const_cast<u8 *>(key.data()),static_cast<int>(key.size())),"Install session key");
                ++radio.owner.authenticated_; std::cout<<"AUTHENTICATED WCID="<<unsigned(radio.wcid)<<"\n";
            }) {}
        void start_auth(){
            ++radio.owner.status_revision_;
            auth_failed=false;auth_stage="negotiating";auth_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);auth.start();
        }
        void fail_auth(const char *stage,const char *reason){
            ++radio.owner.status_revision_;
            auth_failed=true;auth_stage=stage;
            if(audio_connected){radio.owner.audio_output_->disconnect(radio.wcid);audio_connected=false;}
            std::cout<<"AUTH FAILED WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" stage="<<auth_stage<<" peer_version="<<unsigned(auth.peer_version())<<" reason="<<reason<<'\n';
        }
        void dispatch(const GipPacket &p) {
            // Repeated control packets do not bypass the counter publication cadence.
            // Advance the revision at the state transitions handled below.
            if(radio.owner.trace_)std::cout<<"GIP WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" cmd="<<unsigned(p.command)<<" bytes="<<p.payload.size()<<"\n";
            if(p.options&32) {
                switch(p.command) {
                case 2:
                    if(p.payload.size()>=28){
                        auto word=[&](size_t at){return unsigned(p.payload[at])|(unsigned(p.payload[at+1])<<8);};
                        vendor=static_cast<uint16_t>(word(8));product=static_cast<uint16_t>(word(10));
                        std::cout<<"DEVICE WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" vid="<<std::hex<<word(8)<<" pid="<<word(10)<<std::dec<<" firmware=";
                        for(size_t at=12;at<20;at+=2)std::cout<<(at==12?"":".")<<word(at);
                        std::cout<<" hardware=";for(size_t at=20;at<28;at+=2)std::cout<<(at==20?"":".")<<word(at);std::cout<<'\n';
                    }
                    if(!identified)link.send(4);break;
                case 3: {
                    const auto status=parse_battery_status(p.payload);
                    if(id)break; // Accessory status must not disconnect its parent gamepad.
                    if(!status.connected){
                        if(!radio.status_disconnected)++radio.owner.status_revision_;
                        radio.status_disconnected=true;break;
                    }
                    if(!radio.battery || radio.battery->type!=status.type || radio.battery->level!=status.level){
                        ++radio.owner.status_revision_;
                        std::cout<<"BATTERY WCID="<<unsigned(radio.wcid)<<" type="<<unsigned(status.type)<<" level="<<unsigned(status.level)<<"\n";
                    }
                    radio.battery=status;
                    break;
                }
                case 4: {
                    if(identified) break;
                    const auto classes=identification_classes(p.payload);
                    for(const auto &name:classes) std::cout<<"CLASS "<<name<<"\n";
                    for(const auto &guid:identification_interfaces(p.payload))std::cout<<"INTERFACE WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" wire_guid="<<hex(guid)<<'\n';
                    identified=true;
                    ++radio.owner.status_revision_;
                    if(std::find(classes.begin(),classes.end(),"Windows.Xbox.Input.Gamepad")!=classes.end() && !id) {
                        radio.gamepad=true;radio.kind="gamepad";
                        radio.capabilities=identification_capabilities(p.payload);
                        std::cout<<"CAPABILITIES WCID="<<unsigned(radio.wcid)<<" share="<<radio.capabilities.share<<" dli="<<radio.capabilities.dli<<"\n";
                        const std::array<uint8_t,1> power{0}; link.send(5,power);
                        const std::array<uint8_t,3> led{0,1,20}; link.send(10,led); start_auth();
                    } else if(std::find(classes.begin(),classes.end(),"Windows.Xbox.Input.Headset")!=classes.end()) {
                        if(!id)radio.kind="headset";
                        // Narrow compatibility experiment for the observed 2024
                        // Microsoft headset. This is plaintext audio, not a
                        // completed authentication or an installed CCMP key.
                        constexpr std::array<uint8_t,16> pc_opt_out{0x77,0xce,0x34,0x7a,0xe2,0x7d,0xc6,0x45,0x8c,0xa4,0,0x42,0xc0,0x8b,0xd9,0x4a};
                        const auto interfaces=identification_interfaces(p.payload);
                        pc_auth_opt_out=!id && vendor==0x045e && product==0x0b32 && std::find(interfaces.begin(),interfaces.end(),pc_opt_out)!=interfaces.end();
                        if(pc_auth_opt_out){auth_stage="pc-opt-out";std::cout<<"HEADSET PC AUTH OPT OUT WCID="<<unsigned(radio.wcid)<<" plaintext_audio=1\n";}
                        const auto formats=identification_audio_formats(p.payload);
                        for(const auto &f:formats)std::cout<<"AUDIO SUPPORTED WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" in="<<unsigned(f[0])<<" out="<<unsigned(f[1])<<"\n";
                        // Standalone/headset-jack PCM only; legacy chat-headset formats are separate.
                        for(const auto &f:formats){
                            try{audio_format(f[0]);audio_format(f[1]);}catch(const std::exception &){continue;}
                            audio=std::make_unique<HeadsetAudio>(f[0],f[1],std::chrono::steady_clock::now(),
                                [this](uint8_t cmd,std::span<const uint8_t> bytes){link.send(cmd,bytes);std::cout<<"AUDIO CONTROL TX WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" cmd="<<unsigned(cmd)<<" data="<<hex(bytes)<<"\n";},
                                [this]{if(!id && !pc_auth_opt_out)start_auth();});break;
                        }
                        if(!audio)std::cout<<"HEADSET UNSUPPORTED FORMAT WCID="<<unsigned(radio.wcid)<<"\n";
                    } else std::cout<<"Accessory identified; input/audio support pending.\n";
                    break;
                }
                case 6: {
                    if(auth_failed)break;
                    const auto before_version=auth.version(),before_peer=auth.peer_version();
                    if(p.payload.size()>=10 && !(p.payload[1]&1) && (p.payload[6]==2 || p.payload[6]==0x22))
                        std::cout<<"AUTH HELLO RX WCID="<<unsigned(radio.wcid)<<" outer_command="<<unsigned(p.payload[3])<<" command="<<unsigned(p.payload[6])<<" version="<<unsigned(p.payload[7])<<" declared="<<((unsigned(p.payload[8])<<8)|p.payload[9])<<" packet="<<p.payload.size()<<'\n';
                    try{
                        const auto before=auth.version();auth.receive(p.payload);
                        if(before!=auth.version())std::cout<<"AUTH NEGOTIATE WCID="<<unsigned(radio.wcid)<<" requested_version="<<unsigned(auth.version())<<" peer_version="<<unsigned(auth.peer_version())<<'\n';
                        if(auth.complete() && auth_stage!="key-installed"){auth_stage="key-installed";++radio.owner.status_revision_;}
                    }catch(const UnsupportedAuthVersion &error){fail_auth("unsupported-version",error.what());}
                    catch(const AuthProtocolError &error){
                        std::cout<<"AUTH RESPONSE HEADER WCID="<<unsigned(radio.wcid)<<" data="<<hex(std::span(p.payload).first(std::min(size_t(10),p.payload.size())))<<'\n';
                        fail_auth("failed",error.what());
                    }
                    if(before_version!=auth.version() || before_peer!=auth.peer_version())++radio.owner.status_revision_;
                    break;
                }
                case 7: if(p.payload.size()==2 && p.payload[1]==0x5b) {radio.guide=p.payload[0]!=0;radio.publish();std::cout<<"GUIDE "<<unsigned(p.payload[0])<<"\n";} break;
                case 8:
                    if(audio){
                        const auto before=audio->stage();const auto before_input=audio->input().code,before_output=audio->output().code;
                        audio->receive(p.payload,std::chrono::steady_clock::now());
                        if(before!=audio->stage() || before_input!=audio->input().code || before_output!=audio->output().code)++radio.owner.status_revision_;
                        std::cout<<"AUDIO CONTROL RX WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" data="<<hex(p.payload)<<" stage="<<audio->stage_name()<<"\n";
                    }break;
                case 0x60:
                    if(audio && p.payload.size()>=2){
                        ++audio_received;if(audio_received==1 || audio_received%125==0)std::cout<<"AUDIO RX WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" packets="<<audio_received<<" bytes="<<p.payload.size()-2<<"\n";
                        if(audio_connected && audio_allowed() && p.payload.size()==386){radio.owner.audio_output_->capture(radio.wcid,id,std::span(p.payload).subspan(2));++audio_capture;}
                    }break;
                default: break;
                }
            } else if(p.command==32 && !id && identified && radio.gamepad) {
                const auto pad=parse_gamepad(p.payload,radio.capabilities); ++radio.owner.inputs_;
                if(pad.share!=radio.input.share)std::cout<<"SHARE WCID="<<unsigned(radio.wcid)<<" pressed="<<pad.share<<"\n";
                if(radio.input.share!=pad.share)++radio.owner.status_revision_;
                radio.input=pad;radio.publish();
                if(radio.owner.trace_)std::cout<<"INPUT WCID="<<unsigned(radio.wcid)<<" buttons="<<pad.buttons<<" LT="<<pad.left_trigger<<" RT="<<pad.right_trigger<<" LX="<<pad.left_x<<" LY="<<pad.left_y<<" RX="<<pad.right_x<<" RY="<<pad.right_y<<"\n";
            }
        }
        void tick(std::chrono::steady_clock::time_point now){
            if(auth_stage=="negotiating" && now>=auth_deadline)fail_auth("timeout","Handshake did not complete within 15 seconds");
            if(!audio)return;
            const auto before=audio->stage();audio->tick(now);
            if(before!=audio->stage())++radio.owner.status_revision_;
            if(before!=audio->stage())std::cout<<"AUDIO STAGE WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" stage="<<audio->stage_name()<<"\n";
            if(auth_failed || audio->stage()!=HeadsetAudio::Stage::streaming || !audio_allowed() || now<audio_last)return;
            if(!audio_connected && radio.owner.audio_output_ && audio->input().code==9 && audio->output().code==16){
                radio.owner.audio_output_->connect(radio.wcid,id);audio_connected=true;audio_last=std::chrono::steady_clock::now();++radio.owner.status_revision_;
            }
            // One unfragmented PCM packet per 8ms on the dongle audio queue, like upstream.
            // Replace missing playback blocks with silence; microphone data is never saved.
            Bytes samples;if(audio_connected)samples=radio.owner.audio_output_->playback(radio.wcid,id);
            if(samples.empty()){samples.resize(audio->output().interval_bytes);++audio_silence;}else ++audio_playback;
            GipPacket pcm{0x60,static_cast<uint8_t>(32|id),audio_sequence++,0,std::move(samples)};
            if(!audio_sequence)audio_sequence=1;
            const auto sent_at=std::chrono::steady_clock::now();
            if(audio_sent_at!=std::chrono::steady_clock::time_point{}){
                const auto interval=std::chrono::duration_cast<std::chrono::microseconds>(sent_at-audio_sent_at).count();
                audio_interval_min=std::min(audio_interval_min,interval);audio_interval_max=std::max(audio_interval_max,interval);
            }
            if(sent_at-audio_last>std::chrono::milliseconds(2))++audio_late;
            audio_sent_at=sent_at;radio.transmit(pcm,true);++audio_tx;
            audio_write_max=std::max(audio_write_max,std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-sent_at).count());
            audio_last+=std::chrono::milliseconds(8);
            if(now-audio_last>std::chrono::milliseconds(16)){audio_last=now+std::chrono::milliseconds(8);++audio_resets;}
            if(audio_tx%125==0){
                std::cout<<"AUDIO TIMING WCID="<<unsigned(radio.wcid)<<" tx="<<audio_tx<<" interval_min_us="<<audio_interval_min<<" interval_max_us="<<audio_interval_max<<" write_max_us="<<audio_write_max<<" late="<<audio_late<<" clock_resets="<<audio_resets<<" silence="<<audio_silence<<'\n';
                audio_interval_min=INT64_MAX;audio_interval_max=0;audio_write_max=0;
            }
        }
    };
    std::array<std::unique_ptr<Logical>,16> logical;
    Client(Session &s,uint8_t w,Bytes mac):owner(s),wcid(w),address(std::move(mac)) {}
    ControllerStatus::UsbTx usb_tx;
    uint64_t malformed_gip_packets=0;
    void transmit(const GipPacket &p,bool audio=false) {
        const auto encoded=encode_gip(p); const auto packet=wlan_packet(wcid,address,owner.mt_.address,encrypted,encoded,audio);
        const auto started=std::chrono::steady_clock::now();
        owner.check(usb_bulk_msg(owner.mt_.udev,4,const_cast<uint8_t *>(packet.data()),static_cast<int>(packet.size()),nullptr,1000),"GIP transmit");
        usb_tx.last_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
        ++usb_tx.packets;if(usb_tx.last_us>8000)++usb_tx.over_8ms;
        usb_tx.max_us=std::max(usb_tx.max_us,usb_tx.last_us);
    }
    void receive(const GipPacket &p) { const auto id=p.options&15; if(!logical[id]) logical[id]=std::make_unique<Logical>(*this,static_cast<uint8_t>(id)); logical[id]->link.receive(p); }
};
Session::Session(xone_mt76 &mt,InputOutput *output,bool trace,AudioOutput *audio_output,RawGipOutput *raw_output):mt_(mt),output_(output),audio_output_(audio_output),raw_output_(raw_output),trace_(trace) {}
Session::~Session()=default;
bool Session::has_clients()const{return std::any_of(clients_.begin(),clients_.end(),[](const auto &p){return bool(p);});}
void Session::suspend_idle_radio(){
    if(has_clients() || pairing_)throw std::runtime_error("Cannot suspend an occupied radio");
    check(xone_mt76_suspend_radio(&mt_),"Arm radio wake");radio_suspended_=true;
}
void Session::resume_idle_radio(){
    if(!radio_suspended_)return;
    check(xone_mt76_resume_radio(&mt_),"Resume idle radio");radio_suspended_=false;
    pair_scan_last_=std::chrono::steady_clock::now();
}
void Session::enable_idle_channel_scan() {
    idle_channel_scan_=true;pair_scan_last_=std::chrono::steady_clock::now();
    for(size_t i=0;i<XONE_MT_NUM_CHANNELS;++i)if(mt_.channel==&mt_.channels[i]){pair_scan_index_=i;break;}
}
void Session::check(int result,const char *operation) { if(result || mt_.udev->sticky_error) throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(mt_.udev->sticky_error?mt_.udev->sticky_error:result)); }
void Session::pairing(bool enabled) {
    if(enabled)resume_idle_radio();
    if(pairing_==enabled) return;
    check(xone_mt76_set_pairing(&mt_,enabled),"Pairing beacon"); pairing_=enabled;++status_revision_;
    std::cout<<"PAIRING "<<(enabled?"ON":"OFF")<<"\n";
    const bool connected=std::any_of(clients_.begin(),clients_.end(),[](const auto &p){return bool(p);});
    check(xone_mt76_set_led_mode(&mt_,enabled?XONE_MT_LED_BLINK:connected?XONE_MT_LED_ON:XONE_MT_LED_OFF),"Adapter LED");
    if(enabled){
        pair_scan_last_=std::chrono::steady_clock::now();pair_deadline_=pair_scan_last_+std::chrono::seconds(30);
        for(size_t i=0;i<XONE_MT_NUM_CHANNELS;++i)if(mt_.channel==&mt_.channels[i]){pair_scan_index_=i;break;}
    }
}
void Session::remove(uint8_t wcid,const char *reason) { if(wcid && wcid<=16 && clients_[wcid-1]) { if(removals_.size()==32)removals_.erase(removals_.begin());removals_.push_back({hex(clients_[wcid-1]->address),reason,wcid,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())}); if(raw_output_ && raw_output_->target(clients_[wcid-1]->address))raw_output_->event(3,wcid);if(audio_output_)audio_output_->disconnect(wcid);if(output_)output_->disconnect(wcid);clients_[wcid-1].reset();++status_revision_; check(xone_mt76_remove_client(&mt_,wcid),"Remove client"); std::cout<<"DISCONNECTED WCID="<<unsigned(wcid)<<"\n"; } }
void Session::process(const RadioEvent &e) {
    if(e.kind!=RadioEvent::Kind::ignored)resume_idle_radio();
    if(e.kind==RadioEvent::Kind::association || e.kind==RadioEvent::Kind::gip || e.kind==RadioEvent::Kind::client_command)
        pair_scan_last_=std::chrono::steady_clock::now();
    switch(e.kind) {
    case RadioEvent::Kind::button: std::cout<<"ADAPTER BUTTON\n";pairing(true); pair_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(30); break;
    case RadioEvent::Kind::association: {
        for(const auto &c:clients_) if(c && c->address==e.address) return;
        const auto at=std::find_if(clients_.begin(),clients_.end(),[](const auto &c){return !c;});
        if(at==clients_.end()) throw std::runtime_error("Client table full");
        const auto wcid=static_cast<uint8_t>(at-clients_.begin()+1); auto client=std::make_unique<Client>(*this,wcid,e.address);
        check(xone_mt76_associate_client(&mt_,wcid,client->address.data()),"Associate client"); *at=std::move(client);
        ++status_revision_;
        if(raw_output_ && raw_output_->target(e.address))raw_output_->event(1,wcid,e.address);
        if(!pairing_) check(xone_mt76_set_led_mode(&mt_,XONE_MT_LED_ON),"Adapter LED");
        std::cout<<"ASSOCIATED WCID="<<unsigned(wcid)<<" MAC="<<hex(e.address)<<"\n"; break;
    }
    case RadioEvent::Kind::lost: remove(e.wcid,"radio-lost");break;
    case RadioEvent::Kind::disassociation: remove(e.wcid,"radio-disassociation");break;
    case RadioEvent::Kind::client_command:
        if(e.command==1)std::cout<<"PAIR REQUEST MAC="<<hex(e.address)<<" enabled="<<pairing_<<"\n";
        if(e.command==1 && pairing_) {
            // Explicit re-enrollment starts a new radio/authentication session.
            // Otherwise the duplicate-association guard retains the old WCID/key
            // and an exhausted identification exchange indefinitely.
            for(uint8_t w=1;w<=16;++w)if(clients_[w-1] && clients_[w-1]->address==e.address){
                std::cout<<"REPAIR RESET WCID="<<unsigned(w)<<" MAC="<<hex(e.address)<<'\n';remove(w,"explicit-repair");break;
            }
            auto mac=e.address;check(xone_mt76_pair_client(&mt_,mac.data()),"Pair response");pairing(false);
        }
        else if(e.command==16 && e.wcid && e.wcid<=16 && clients_[e.wcid-1]) {
            auto &client=*clients_[e.wcid-1];
            if(!client.logical[0] || !client.logical[0]->auth.complete()){
                if(client.logical[0] && !client.logical[0]->auth_failed)client.logical[0]->fail_auth("failed","Encryption before authentication");
                std::cout<<"ENCRYPTION REJECTED WCID="<<unsigned(e.wcid)<<'\n';break;
            }
            std::array<uint8_t,2> zeros{}; check(xone_mt76_send_client_command(&mt_,e.wcid,client.address.data(),XONE_MT_CLIENT_ENABLE_ENCRYPTION,zeros.data(),2),"Enable encryption");
            if(!client.encrypted || client.logical[0]->auth_stage!="encrypted")++status_revision_;
            client.encrypted=true;
            std::cout<<"ENCRYPTED WCID="<<unsigned(e.wcid)<<"\n";
            client.logical[0]->auth_stage="encrypted";
            if(output_ && client.gamepad)output_->connect(e.wcid);client.publish();client.rumble(0,0);
        } break;
    case RadioEvent::Kind::gip:
        if(e.wcid && e.wcid<=16 && clients_[e.wcid-1]) {
            auto &client=*clients_[e.wcid-1];
            if(!client.output_error.empty())break;
            std::vector<GipPacket> packets;
            // Parse the complete radio transfer before delivering any packet.
            // Keep dispatch outside the catch: transport/output failures remain fatal.
            try { packets=parse_gip(e.payload); }
            catch(const std::runtime_error &) { ++client.malformed_gip_packets; break; }
            if(raw_output_ && raw_output_->target(clients_[e.wcid-1]->address)){
                // Audio has a separate IPC type and never enters interrupt IN.
                for(const auto &p:packets)raw_output_->event(p.command==0x60?4:2,e.wcid,encode_gip(p));
            }else for(const auto &p:packets) client.receive(p);
        }break;
    default: break;
    }
}
void Session::tick(std::chrono::steady_clock::time_point now) {
    auto requests=raw_output_?raw_output_->poll():std::vector<RawGipTransmit>{};
    if(raw_output_)for(auto &fault:raw_output_->faults()){
        if(!fault.wcid || fault.wcid>16 || !clients_[fault.wcid-1])continue;
        auto &client=*clients_[fault.wcid-1];if(!client.output_error.empty())continue;
        if(!output_retries_.contains(client.address) && output_retries_.size()>=64)output_retries_.erase(output_retries_.begin());
        auto &retry=output_retries_[client.address];
        if(now-client.created>=std::chrono::seconds(60) || now-retry.last>=std::chrono::minutes(5))retry.attempts=0;
        retry.attempts=std::min(retry.attempts+1,6u);retry.last=now;
        const auto delay=std::min(1u<<(retry.attempts-1),30u);
        client.output_error=fault.error.empty()?"USB route failed":std::move(fault.error);
        client.output_recovery_attempt=retry.attempts;client.output_retry=now+std::chrono::seconds(delay);++status_revision_;
        diagnostic("output_route_failed",{{"wcid",fault.wcid},{"error",client.output_error},{"attempt",retry.attempts},{"retry_seconds",delay}},true);
    }
    prioritize_control(requests);
    for(const auto &request:requests){
        if(!request.wcid || request.wcid>16 || !clients_[request.wcid-1] || !clients_[request.wcid-1]->output_error.empty() || !raw_output_->target(clients_[request.wcid-1]->address)){
            raw_output_->complete(request.wcid,request.token,-19);continue;
        }
        std::vector<GipPacket> packets;
        try{packets=parse_gip(request.data);}catch(const std::exception &){raw_output_->complete(request.wcid,request.token,-71);continue;}
        const bool invalid_audio=request.audio && (packets.size()!=1 || packets[0].command!=0x60 || packets[0].options!=32 || !packets[0].sequence || packets[0].payload.size()!=1536);
        const bool invalid_data=!request.audio && (request.data.size()>64 || std::any_of(packets.begin(),packets.end(),[](const auto &p){return p.command==0x60;}));
        if(packets.empty() || invalid_audio || invalid_data){
            raw_output_->complete(request.wcid,request.token,-95);continue;
        }
        if(request.audio && std::chrono::steady_clock::now()>request.media_deadline){
            raw_output_->complete(request.wcid,request.token,-18);continue;
        }
        for(const auto &p:packets)clients_[request.wcid-1]->transmit(p,request.audio);
        raw_output_->complete(request.wcid,request.token,0);
    }
    // Defer removal until dispatch/GipLink no longer owns the active client stack.
    for(uint8_t w=1;w<=16;++w)if(clients_[w-1] && !clients_[w-1]->output_error.empty() && now>=clients_[w-1]->output_retry){
        diagnostic("output_route_recovery_reset",{{"wcid",w},{"attempt",clients_[w-1]->output_recovery_attempt}},true);
        remove(w,"output-route-failed");
    }
    for(uint8_t w=1;w<=16;++w)if(clients_[w-1] && clients_[w-1]->status_disconnected)remove(w,"gip-status-disconnected");
    if(pairing_ && now>=pair_deadline_)pairing(false);
    // Scan only with an empty client table: changing channel during auth/audio breaks it.
    if(!radio_suspended_ && (pairing_ || idle_channel_scan_) && now-pair_scan_last_>=std::chrono::seconds(2) &&
       std::none_of(clients_.begin(),clients_.end(),[](const auto &c){return bool(c);})){
        const auto next=(pair_scan_index_+1)%XONE_MT_NUM_CHANNELS;
        auto &channel=mt_.channels[next];
        if(channel.index){
            check(xone_mt76_switch_channel(&mt_,&channel),"Pairing channel switch");mt_.channel=&channel;
            ++status_revision_;
            check(xone_mt76_set_pairing(&mt_,pairing_),"Channel beacon");
            std::cout<<(pairing_?"PAIRING CHANNEL ":"RECONNECT CHANNEL ")<<unsigned(channel.index)<<"\n";
        }
        pair_scan_index_=next;pair_scan_last_=now;
    }
    if(output_)for(unsigned n=0;n<16;++n) {
        const auto f=output_->feedback();if(!f)break;
        if(f->wcid && f->wcid<=16 && clients_[f->wcid-1]) {auto &c=*clients_[f->wcid-1];c.pending_rumble=f->packet.value_or(rumble_packet(f->large,f->small_motor));c.rumble_pending=true;}
    }
    for(const auto &c:clients_)if(c && c->rumble_pending && now-c->rumble_last>=std::chrono::milliseconds(10))c->rumble(c->pending_rumble);
    for(const auto &c:clients_)if(c){
        if(raw_output_ && raw_output_->target(c->address))continue;
        if(c->identify_attempts<5 && now>=c->identify_retry && (!c->logical[0] || !c->logical[0]->identified)){
            if(!c->logical[0])c->logical[0]=std::make_unique<Client::Logical>(*c,uint8_t(0));
            c->logical[0]->link.send(4);++c->identify_attempts;c->identify_retry=now+std::chrono::seconds(1);
            std::cout<<"IDENTIFY RETRY WCID="<<unsigned(c->wcid)<<" attempt="<<c->identify_attempts<<'\n';
        }
        for(const auto &l:c->logical)if(l)l->tick(now);
    }
}
std::vector<ControllerStatus> Session::status() const {
    std::vector<ControllerStatus> out;
    for(const auto &c:clients_)if(c){
        ControllerStatus s{c->wcid,hex(c->address),c->encrypted,c->input.share,c->capabilities,c->battery,c->kind,{},"not-started",0,0,c->usb_tx};
        s.malformed_gip_packets=c->malformed_gip_packets;
        s.output_recovering=!c->output_error.empty();s.output_recovery_attempt=c->output_recovery_attempt;s.output_error=c->output_error;
        if(c->logical[0]){const auto &l=*c->logical[0];s.authentication=l.auth_stage;s.auth_version=l.auth.version();s.peer_auth_version=l.auth.peer_version();}
        for(const auto &l:c->logical)if(l && l->audio){s.audio=ControllerStatus::Audio{l->auth_failed?"authentication-failed":l->audio_connected?"streaming-native":l->audio->stage_name(),l->audio->input().rate,l->audio->output().rate,l->audio->input().channels,l->audio->output().channels,l->audio_received,l->audio_silence,l->audio_playback,l->audio_capture,l->audio_connected?audio_output_->endpoints(c->wcid,l->id):AudioEndpoints{},l->audio_connected};break;}
        out.push_back(std::move(s));
    }
    return out;
}
bool Session::audio_active() const {
    for(const auto &c:clients_)if(c)for(const auto &l:c->logical)if(l && !l->auth_failed && l->audio_allowed() && l->audio && l->audio->stage()==HeadsetAudio::Stage::streaming)return true;
    return false;
}
void Session::close() {
    resume_idle_radio();
    pairing(false);
    for(uint8_t w=1;w<=16;++w) {
        if(clients_[w-1] && clients_[w-1]->encrypted)try{clients_[w-1]->rumble(0,0);}catch(...){}
        remove(w);
    }
    check(xone_mt76_set_led_mode(&mt_,XONE_MT_LED_OFF),"Adapter LED cleanup");
}
}

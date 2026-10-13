// SPDX-License-Identifier: GPL-2.0-or-later
// Port of xone transport/dongle.c, Copyright (C) 2021 Severin von Wnuck-Lipinski.
#include "session.hpp"
#include "headset_audio.hpp"
#include "radio_audio_clock.hpp"
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
    bool raw_gip_seen=false;
    unsigned raw_recovery_attempts=0;
    std::chrono::steady_clock::time_point associated_at=std::chrono::steady_clock::now();
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
        RadioAudioClock audio_clock;
        bool audio_allowed() const{return radio.encrypted || pc_auth_opt_out;}
        Logical(Client &c,uint8_t client_id):radio(c),id(client_id),link(id,[this](const GipPacket &p){radio.transmit(p);},[this](const GipPacket &p){dispatch(p);}),
            auth([this](std::span<const uint8_t> b,bool ack){link.send(6,b,ack);},[this](std::span<const uint8_t> key){
                if(id) throw std::runtime_error("Encryption requested by accessory");
                radio.owner.check(xone_mt76_set_client_key(&radio.owner.mt_,radio.wcid,const_cast<u8 *>(key.data()),static_cast<int>(key.size())),"Install session key");
                ++radio.owner.authenticated_; std::cout<<"AUTHENTICATED WCID="<<unsigned(radio.wcid)<<"\n";
            }) {}
        void start_auth(){
            auth_failed=false;auth_stage="negotiating";auth_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);auth.start();
        }
        void fail_auth(const char *stage,const char *reason){
            auth_failed=true;auth_stage=stage;
            if(audio_connected){radio.owner.audio_output_->disconnect(radio.wcid);audio_connected=false;}
            std::cout<<"AUTH FAILED WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" stage="<<auth_stage<<" peer_version="<<unsigned(auth.peer_version())<<" reason="<<reason<<'\n';
        }
        void dispatch(const GipPacket &p) {
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
                    if(!status.connected){radio.status_disconnected=true;break;}
                    if(!radio.battery || radio.battery->type!=status.type || radio.battery->level!=status.level)
                        std::cout<<"BATTERY WCID="<<unsigned(radio.wcid)<<" type="<<unsigned(status.type)<<" level="<<unsigned(status.level)<<"\n";
                    radio.battery=status;
                    break;
                }
                case 4: {
                    if(identified) break;
                    const auto classes=identification_classes(p.payload);
                    for(const auto &name:classes) std::cout<<"CLASS "<<name<<"\n";
                    for(const auto &guid:identification_interfaces(p.payload))std::cout<<"INTERFACE WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" wire_guid="<<hex(guid)<<'\n';
                    identified=true;
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
                        if(pc_auth_opt_out){auth_failed=false;auth_stage="pc-opt-out";std::cout<<"HEADSET PC AUTH OPT OUT WCID="<<unsigned(radio.wcid)<<" plaintext_audio=1\n";}
                        auto formats=identification_audio_formats(p.payload);
                        // Synthetic Windows audio consumes mono 24k and stereo 48k.
                        // Prefer this supported pair over the headset's legacy 16k chat mode.
                        std::stable_sort(formats.begin(),formats.end(),[](const auto &a,const auto &b){return (a==std::array<uint8_t,2>{9,16})>(b==std::array<uint8_t,2>{9,16});});
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
                case 6:
                    if(auth_failed || pc_auth_opt_out)break;
                    if(p.payload.size()>=10 && !(p.payload[1]&1) && (p.payload[6]==2 || p.payload[6]==0x22))
                        std::cout<<"AUTH HELLO RX WCID="<<unsigned(radio.wcid)<<" outer_command="<<unsigned(p.payload[3])<<" command="<<unsigned(p.payload[6])<<" version="<<unsigned(p.payload[7])<<" declared="<<((unsigned(p.payload[8])<<8)|p.payload[9])<<" packet="<<p.payload.size()<<'\n';
                    try{
                        const auto before=auth.version();auth.receive(p.payload);
                        if(before!=auth.version())std::cout<<"AUTH NEGOTIATE WCID="<<unsigned(radio.wcid)<<" requested_version="<<unsigned(auth.version())<<" peer_version="<<unsigned(auth.peer_version())<<'\n';
                        if(auth.complete())auth_stage="key-installed";
                    }catch(const UnsupportedAuthVersion &error){fail_auth("unsupported-version",error.what());}
                    catch(const AuthProtocolError &error){
                        std::cout<<"AUTH RESPONSE HEADER WCID="<<unsigned(radio.wcid)<<" data="<<hex(std::span(p.payload).first(std::min(size_t(10),p.payload.size())))<<'\n';
                        fail_auth("failed",error.what());
                    }
                    break;
                case 7: if(p.payload.size()==2 && p.payload[1]==0x5b) {radio.guide=p.payload[0]!=0;radio.publish();std::cout<<"GUIDE "<<unsigned(p.payload[0])<<"\n";} break;
                case 8:
                    if(audio){audio->receive(p.payload,std::chrono::steady_clock::now());std::cout<<"AUDIO CONTROL RX WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" data="<<hex(p.payload)<<" stage="<<audio->stage_name()<<"\n";}break;
                case 0x60:
                    if(audio && audio_clock.receive(p.payload)){
                        ++audio_received;
                        if(radio.owner.trace_ && (audio_received==1 || audio_received%125==0))std::cout<<"AUDIO RX WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" packets="<<audio_received<<" bytes="<<p.payload.size()-2<<"\n";
                        if(audio_connected && audio_allowed()){radio.owner.audio_output_->capture(radio.wcid,id,std::span(p.payload).subspan(2));++audio_capture;}
                    }break;
                default: break;
                }
            } else if(p.command==32 && !id && identified && radio.gamepad) {
                const auto pad=parse_gamepad(p.payload,radio.capabilities); ++radio.owner.inputs_;
                if(pad.share!=radio.input.share)std::cout<<"SHARE WCID="<<unsigned(radio.wcid)<<" pressed="<<pad.share<<"\n";
                radio.input=pad;radio.publish();
                if(radio.owner.trace_)std::cout<<"INPUT WCID="<<unsigned(radio.wcid)<<" buttons="<<pad.buttons<<" LT="<<pad.left_trigger<<" RT="<<pad.right_trigger<<" LX="<<pad.left_x<<" LY="<<pad.left_y<<" RX="<<pad.right_x<<" RY="<<pad.right_y<<"\n";
            }
        }
        void tick(std::chrono::steady_clock::time_point now){
            if(auth_stage=="negotiating" && now>=auth_deadline)fail_auth("timeout","Handshake did not complete within 15 seconds");
            if(!audio)return;
            const auto before=audio->stage();audio->tick(now);
            if(before!=audio->stage())std::cout<<"AUDIO STAGE WCID="<<unsigned(radio.wcid)<<" client="<<unsigned(id)<<" stage="<<audio->stage_name()<<"\n";
            if(auth_failed || audio->stage()!=HeadsetAudio::Stage::streaming || !audio_allowed())return;
            if(!audio_connected && radio.owner.audio_output_ && audio->input().code==9 && audio->output().code==16){
                radio.owner.audio_output_->connect(radio.wcid,id);audio_connected=true;
            }
            if(!audio_clock.take())return;
            // Send on the physical headset clock, as in the working USB/GIP path.
            // Replace missing playback blocks with silence; microphone data is never saved.
            Bytes samples;if(audio_connected)samples=radio.owner.audio_output_->playback(radio.wcid,id);
            if(samples.empty()){samples.resize(audio->output().interval_bytes);++audio_silence;}else ++audio_playback;
            GipPacket pcm{0x60,static_cast<uint8_t>(32|id),audio_sequence++,0,std::move(samples)};
            if(!audio_sequence)audio_sequence=1;
            radio.transmit(pcm,true);
        }
    };
    std::array<std::unique_ptr<Logical>,16> logical;
    Client(Session &s,uint8_t w,Bytes mac):owner(s),wcid(w),address(std::move(mac)) {}
    void transmit(const GipPacket &p,bool audio=false) {
        const auto encoded=encode_gip(p); const auto packet=wlan_packet(wcid,address,owner.mt_.address,encrypted,encoded,audio);
        owner.check(usb_bulk_msg(owner.mt_.udev,4,const_cast<uint8_t *>(packet.data()),static_cast<int>(packet.size()),nullptr,1000),"GIP transmit");
    }
    void receive(const GipPacket &p) { const auto id=p.options&15; if(!logical[id]) logical[id]=std::make_unique<Logical>(*this,static_cast<uint8_t>(id)); logical[id]->link.receive(p); }
};
Session::Session(xone_mt76 &mt,InputOutput *output,bool trace,AudioOutput *audio_output,RawGipOutput *raw_output):mt_(mt),output_(output),audio_output_(audio_output),raw_output_(raw_output),trace_(trace) {}
Session::~Session()=default;
void Session::enable_native_reconnect(){native_reconnect_=true;}
void Session::remember_peer(std::span<const uint8_t> address){
    if(address.size()!=6 || std::all_of(address.begin(),address.end(),[](uint8_t b){return !b;}))throw std::runtime_error("Invalid native peer address");
    if(std::any_of(native_peers_.begin(),native_peers_.end(),[&](const auto &p){return std::equal(p.begin(),p.end(),address.begin(),address.end());}))return;
    if(native_peers_.size()>=16)throw std::runtime_error("Native peer table full");
    native_peers_.emplace_back(address.begin(),address.end());
}
std::vector<std::string> Session::known_peers()const{std::vector<std::string> out;for(const auto &p:native_peers_)out.push_back(hex(p));return out;}
bool Session::known_peer(std::span<const uint8_t> address)const{
    return (raw_output_ && raw_output_->target(address)) || (native_reconnect_ && std::any_of(native_peers_.begin(),native_peers_.end(),[&](const auto &p){return std::equal(p.begin(),p.end(),address.begin(),address.end());}));
}
void Session::check(int result,const char *operation) { if(result || mt_.udev->sticky_error) throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(mt_.udev->sticky_error?mt_.udev->sticky_error:result)); }
void Session::pairing(bool enabled) {
    if(pairing_==enabled) return;
    // Configured USB peers may re-enroll after power-on instead of remembering
    // their radio association. Advertise discovery but keep manual enrollment
    // separate: only configured addresses receive automatic pair responses.
    check(xone_mt76_set_pairing(&mt_,enabled || raw_output_ || (native_reconnect_ && !native_peers_.empty())),"Pairing beacon"); pairing_=enabled;
    std::cout<<"PAIRING "<<(enabled?"ON":"OFF")<<"\n";
    const bool connected=std::any_of(clients_.begin(),clients_.end(),[](const auto &p){return bool(p);});
    check(xone_mt76_set_led_mode(&mt_,enabled?XONE_MT_LED_BLINK:connected?XONE_MT_LED_ON:XONE_MT_LED_OFF),"Adapter LED");
    if(enabled){
        pair_scan_last_=std::chrono::steady_clock::now();pair_deadline_=pair_scan_last_+std::chrono::seconds(30);
        for(size_t i=0;i<XONE_MT_NUM_CHANNELS;++i)if(mt_.channel==&mt_.channels[i]){pair_scan_index_=i;break;}
    }
}
void Session::remove(uint8_t wcid) { if(wcid && wcid<=16 && clients_[wcid-1]) { if(raw_output_ && raw_output_->target(clients_[wcid-1]->address))raw_output_->event(3,wcid);if(audio_output_)audio_output_->disconnect(wcid);if(output_)output_->disconnect(wcid);clients_[wcid-1].reset(); check(xone_mt76_remove_client(&mt_,wcid),"Remove client"); std::cout<<"DISCONNECTED WCID="<<unsigned(wcid)<<"\n"; } }
void Session::start_reconnect_window() {
    // dlundqvist/xone 3f07071: the closed RX filter blocks initial association.
    // Two complete 12-channel scans fit into this startup window.
    pairing(true);
    pair_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    std::cout<<"STARTUP RECONNECT WINDOW seconds=60\n";
}
void Session::recover_raw_client(uint8_t wcid,std::chrono::steady_clock::time_point now) {
    auto &old=clients_[wcid-1];
    const auto address=old->address;
    const auto attempt=old->raw_gip_seen?0u:old->raw_recovery_attempts+1;
    // Invalidate the USB generation before clearing the firmware's peer/key slot.
    remove(wcid);
    auto client=std::make_unique<Client>(*this,wcid,address);
    client->associated_at=now;client->raw_recovery_attempts=attempt;
    check(xone_mt76_associate_client(&mt_,wcid,client->address.data()),"Recover association");
    clients_[wcid-1]=std::move(client);
    if(raw_output_ && raw_output_->target(address))raw_output_->event(1,wcid,address);
    std::cout<<"SESSION RECOVERY WCID="<<unsigned(wcid)<<" attempt="<<attempt<<'\n';
}
void Session::process(const RadioEvent &e,std::chrono::steady_clock::time_point now) {
    if(e.kind==RadioEvent::Kind::association || e.kind==RadioEvent::Kind::gip || e.kind==RadioEvent::Kind::client_command)
        pair_scan_last_=now;
    switch(e.kind) {
    case RadioEvent::Kind::button:
        std::cout<<"ADAPTER BUTTON\n";pairing(true);
        // A repeated button event must not let the next tick rotate the channel
        // while the user is starting a peer handshake (upstream 940cf37).
        pair_scan_last_=std::chrono::steady_clock::now();
        pair_deadline_=pair_scan_last_+std::chrono::seconds(60);break;
    case RadioEvent::Kind::association: {
        if(raw_output_ && !pairing_ && !output_ && !audio_output_ && !raw_output_->target(e.address))return;
        if(native_reconnect_ && !pairing_ && !known_peer(e.address))return;
        for(const auto &c:clients_) if(c && c->address==e.address) {
            // Debounce immediate WLAN retries. A later association from a known
            // USB peer must not inherit the previous Windows/GIP generation.
            if(known_peer(e.address) &&
               now-c->associated_at>=std::chrono::seconds(1) &&
               (c->raw_gip_seen || c->raw_recovery_attempts<3))recover_raw_client(c->wcid,now);
            return;
        }
        const auto at=std::find_if(clients_.begin(),clients_.end(),[](const auto &c){return !c;});
        if(at==clients_.end()) throw std::runtime_error("Client table full");
        const auto wcid=static_cast<uint8_t>(at-clients_.begin()+1); auto client=std::make_unique<Client>(*this,wcid,e.address);
        client->associated_at=now;
        check(xone_mt76_associate_client(&mt_,wcid,client->address.data()),"Associate client"); *at=std::move(client);
        association_pending_until_={};
        if(raw_output_ && raw_output_->target(e.address))raw_output_->event(1,wcid,e.address);
        if(!pairing_) check(xone_mt76_set_led_mode(&mt_,XONE_MT_LED_ON),"Adapter LED");
        std::cout<<"ASSOCIATED WCID="<<unsigned(wcid)<<" MAC="<<hex(e.address)<<"\n"; break;
    }
    case RadioEvent::Kind::lost: case RadioEvent::Kind::disassociation: remove(e.wcid); break;
    case RadioEvent::Kind::client_command:
        if(e.command==1)std::cout<<"PAIR REQUEST MAC="<<hex(e.address)<<" enabled="<<pairing_<<"\n";
        if(e.command==1 && (pairing_ || known_peer(e.address))) {
            // Upstream answers PAIR_REQ outside the button window as well.
            // Limit automatic re-enrollment here to explicitly configured peers;
            // unknown devices still require the adapter pairing button.
            if(!pairing_)std::cout<<"KNOWN PEER RECONNECT MAC="<<hex(e.address)<<'\n';
            // Explicit re-enrollment starts a new radio/authentication session.
            // Otherwise the duplicate-association guard retains the old WCID/key
            // and an exhausted identification exchange indefinitely.
            for(uint8_t w=1;w<=16;++w)if(clients_[w-1] && clients_[w-1]->address==e.address){
                std::cout<<"REPAIR RESET WCID="<<unsigned(w)<<" MAC="<<hex(e.address)<<'\n';remove(w);break;
            }
            auto mac=e.address;check(xone_mt76_pair_client(&mt_,mac.data()),"Pair response");if(native_reconnect_)remember_peer(mac);pairing(false);
            // Discovery must not leave this channel before the peer completes
            // re-enrollment. The old headset can take longer than one scan dwell.
            association_pending_until_=now+std::chrono::seconds(10);
        }
        else if(e.command==16 && e.wcid && e.wcid<=16 && clients_[e.wcid-1]) {
            auto &client=*clients_[e.wcid-1];
            if(!client.logical[0] || !client.logical[0]->auth.complete()){
                if(client.logical[0] && !client.logical[0]->auth_failed)client.logical[0]->fail_auth("failed","Encryption before authentication");
                std::cout<<"ENCRYPTION REJECTED WCID="<<unsigned(e.wcid)<<'\n';break;
            }
            std::array<uint8_t,2> zeros{}; check(xone_mt76_send_client_command(&mt_,e.wcid,client.address.data(),XONE_MT_CLIENT_ENABLE_ENCRYPTION,zeros.data(),2),"Enable encryption"); client.encrypted=true;
            std::cout<<"ENCRYPTED WCID="<<unsigned(e.wcid)<<"\n";
            client.logical[0]->auth_stage="encrypted";
            if(output_ && client.gamepad)output_->connect(e.wcid);client.publish();client.rumble(0,0);
        } break;
    case RadioEvent::Kind::gip:
        if(e.wcid && e.wcid<=16 && clients_[e.wcid-1]) {
            if(raw_output_ && raw_output_->target(clients_[e.wcid-1]->address)){
                const auto packets=parse_gip(e.payload);
                if(!packets.empty())clients_[e.wcid-1]->raw_gip_seen=true;
                // Audio has a separate IPC type and never enters interrupt IN.
                for(const auto &p:packets)raw_output_->event(p.command==0x60?4:2,e.wcid,encode_gip(p));
            }else {const auto packets=parse_gip(e.payload);if(!packets.empty())clients_[e.wcid-1]->raw_gip_seen=true;for(const auto &p:packets)clients_[e.wcid-1]->receive(p);}
        }break;
    default: break;
    }
}
void Session::tick(std::chrono::steady_clock::time_point now) {
    if(raw_output_)for(const auto &request:raw_output_->poll()){
        if(!request.wcid || request.wcid>16 || !clients_[request.wcid-1] || !raw_output_->target(clients_[request.wcid-1]->address)){
            raw_output_->complete(request.wcid,request.token,-19);continue;
        }
        std::vector<GipPacket> packets;
        try{packets=parse_gip(request.data);}catch(const std::exception &){raw_output_->complete(request.wcid,request.token,-71);continue;}
        const bool invalid_audio=request.audio && (packets.size()!=1 || packets[0].command!=0x60 || packets[0].options!=32 || !packets[0].sequence || packets[0].payload.size()!=1536);
        const bool invalid_data=!request.audio && (request.data.size()>64 || std::any_of(packets.begin(),packets.end(),[](const auto &p){return p.command==0x60;}));
        if(packets.empty() || invalid_audio || invalid_data){
            raw_output_->complete(request.wcid,request.token,-95);continue;
        }
        for(const auto &p:packets)clients_[request.wcid-1]->transmit(p,request.audio);
        raw_output_->complete(request.wcid,request.token,0);
    }
    // Defer removal until dispatch/GipLink no longer owns the active client stack.
    for(uint8_t w=1;w<=16;++w)if(clients_[w-1] && clients_[w-1]->status_disconnected)remove(w);
    // Recover a silent initial association, never an established but idle peer.
    // No global firmware reset/channel change: other controllers/audio keep going.
    for(uint8_t w=1;w<=16;++w){
        const auto &c=clients_[w-1];
        if(c && known_peer(c->address) && !c->raw_gip_seen &&
           c->raw_recovery_attempts<3 && now-c->associated_at>=std::chrono::seconds(5))
            recover_raw_client(w,now);
    }
    if(pairing_ && now>=pair_deadline_)pairing(false);
    // Scan only with an empty client table: changing channel during auth/audio breaks it.
    if((pairing_ || raw_output_ || (native_reconnect_ && !native_peers_.empty())) && now>=association_pending_until_ && now-pair_scan_last_>=std::chrono::seconds(2) &&
       std::none_of(clients_.begin(),clients_.end(),[](const auto &c){return bool(c);})){
        const auto next=(pair_scan_index_+1)%XONE_MT_NUM_CHANNELS;
        auto &channel=mt_.channels[next];
        if(channel.index){
            check(xone_mt76_switch_channel(&mt_,&channel),"Pairing channel switch");mt_.channel=&channel;
            check(xone_mt76_set_pairing(&mt_,true),"Pairing channel beacon");
            std::cout<<"PAIRING CHANNEL "<<unsigned(channel.index)<<"\n";
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
        ControllerStatus s{c->wcid,hex(c->address),c->encrypted,c->input.share,c->capabilities,c->battery,c->kind,{},"not-started",0,0};
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
    pairing(false);
    if(raw_output_ || native_reconnect_)check(xone_mt76_set_pairing(&mt_,false),"Reconnect beacon cleanup");
    for(uint8_t w=1;w<=16;++w) {
        if(clients_[w-1] && clients_[w-1]->encrypted)try{clients_[w-1]->rumble(0,0);}catch(...){}
        remove(w);
    }
    check(xone_mt76_set_led_mode(&mt_,XONE_MT_LED_OFF),"Adapter LED cleanup");
}
}

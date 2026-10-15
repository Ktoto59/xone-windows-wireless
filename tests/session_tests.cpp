// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "session.hpp"
#include "raw_gip_router.hpp"
#include "headset_audio.hpp"
#include "audio_ipc.hpp"
#include "synthetic_audio.hpp"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <deque>
using namespace xone;
static void check(bool ok,const char *message) { if(!ok) throw std::runtime_error(message); }
template<class F> static void rejects(F f) { bool threw=false; try{f();}catch(const std::exception &){threw=true;} check(threw,"Malformed input accepted"); }
static Bytes unhex(std::string_view s) { Bytes b; for(size_t i=0;i<s.size();i+=2) b.push_back(static_cast<uint8_t>(std::stoul(std::string(s.substr(i,2)),nullptr,16))); return b; }
static Bytes scalar_blob(uint8_t scalar,std::span<const uint8_t> pub) {
    BCRYPT_ECCKEY_BLOB h{BCRYPT_ECDH_PRIVATE_P256_MAGIC,32}; Bytes b(sizeof(h)); std::memcpy(b.data(),&h,sizeof(h)); b.insert(b.end(),pub.begin(),pub.end()); b.resize(104); b.back()=scalar; return b;
}
static Bytes auth_response(uint8_t cmd,const Bytes &data) {
    const auto n=static_cast<uint16_t>(data.size()),total=static_cast<uint16_t>(n+4);
    Bytes p{0,0xc0,0,cmd,static_cast<uint8_t>(total>>8),static_cast<uint8_t>(total),cmd,static_cast<uint8_t>(cmd>=0x21?2:1),static_cast<uint8_t>(n>>8),static_cast<uint8_t>(n)};
    p.insert(p.end(),data.begin(),data.end()); return p;
}
static void handshake(bool v2,bool corrupt,bool v3_probe=false) {
    const auto g=unhex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    const auto two_g=unhex("7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc4766997807775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1");
    Bytes transcript,sent,key,master,seed; BCRYPT_KEY_HANDLE rsa=nullptr;
    struct Cleanup { BCRYPT_KEY_HANDLE &key; ~Cleanup(){if(key) BCryptDestroyKey(key);} } cleanup{rsa};
    Auth auth([&](std::span<const uint8_t> b,bool){
        sent.assign(b.begin(),b.end()); if(b.size()>14 && b[0]==0 && b[1]==0x41) transcript.insert(transcript.end(),b.begin()+6,b.end()-8);
    },[&](std::span<const uint8_t> b){key.assign(b.begin(),b.end());});
    const Bytes ack{0,0xc1,0,1,0,0}; auth.start();
    if(v2) { auth.receive(ack); auto upgrade=auth_response(2,Bytes(v3_probe?0:80)); upgrade[3]=v3_probe?2:0x22; upgrade[7]=v3_probe?3:2; transcript.clear(); auth.receive(upgrade); check(sent[3]==0x21 && !auth.complete() && key.empty(),"Auth2 upgrade failed or bypassed authentication"); }
    seed.assign(sent.begin()+10,sent.begin()+42); seed.resize(64,0x55);
    auth.receive(ack); check(sent[3]==(v2?0x22:2),"Wrong hello request");
    auto respond=[&](uint8_t cmd,Bytes data) { auto p=auth_response(cmd,data); transcript.insert(transcript.end(),p.begin()+6,p.end()); auth.receive(p); };
    Bytes hello(v2?172:80); std::fill(hello.begin(),hello.begin()+32,uint8_t(0x55)); respond(v2?0x22:2,hello);
    if(v2) {
        respond(0x23,Bytes(768)); Bytes pub=two_g; pub.resize(128); respond(0x24,pub);
        Bytes unused; const auto secret=auth_ecdh(std::span<const uint8_t>(sent).subspan(10,64),unused,scalar_blob(2,two_g));
        master=auth_prf("Master Secret",secret,seed,48);
    } else {
        check(BCryptGenerateKeyPair(BCRYPT_RSA_ALG_HANDLE,&rsa,2048,0)>=0 && BCryptFinalizeKeyPair(rsa,0)>=0,"RSA generate");
        Bytes blob(512); ULONG n=0; check(BCryptExportKey(rsa,nullptr,BCRYPT_RSAPUBLIC_BLOB,blob.data(),static_cast<ULONG>(blob.size()),&n,0)>=0,"RSA export");
        BCRYPT_RSAKEY_BLOB h{}; std::memcpy(&h,blob.data(),sizeof(h)); check(h.cbPublicExp==3 && h.cbModulus==256,"RSA layout");
        Bytes cert{0x30,0x82,1,0x0a,2,0x82,1,1,0}; cert.insert(cert.end(),blob.begin()+sizeof(h)+3,blob.begin()+sizeof(h)+3+256); cert.push_back(2);cert.push_back(3);cert.insert(cert.end(),blob.begin()+sizeof(h),blob.begin()+sizeof(h)+3);
        respond(3,cert); Bytes pms(48);
        check(BCryptDecrypt(rsa,sent.data()+10,256,nullptr,nullptr,0,pms.data(),48,&n,BCRYPT_PAD_PKCS1)>=0 && n==48,"RSA decrypt premaster"); master=auth_prf("Master Secret",pms,seed,48);
    }
    const auto before=transcript; auth.receive(ack);
    check(sent[3]==(v2?0x26:7),"Host finish command");
    const auto wanted=auth_prf("Host Finished",master,hash256(before),32);
    check(std::equal(wanted.begin(),wanted.end(),sent.begin()+10),"Host transcript mismatch");
    auth.receive(ack); Bytes finish=auth_prf("Device Finished",master,hash256(transcript),32); finish.resize(64); if(corrupt) finish[0]^=1;
    const auto response=auth_response(v2?0x27:8,finish);
    if(corrupt) { rejects([&]{auth.receive(response);}); check(key.empty() && !auth.complete(),"Key installed on failed auth"); }
    else { auth.receive(response); check(auth.complete() && sent==Bytes({1,0}),"Auth completion missing"); check(key==auth_prf("EXPORTER DAWN data channel session key for controller",master,seed,16),"Session exporter mismatch"); }
}
int main() {
    unsigned passed=0;
    auto test=[&](const char *name,auto f){f();++passed;std::cout<<"PASS "<<name<<"\n";};
    try {
        test("GIP header roundtrip and even alignment",[]{
            for(uint32_t n:{0u,1u,58u,128u,1024u}) for(uint32_t off:{0u,58u,128u,65535u}) {
                GipPacket p{6,0xa0,5,off,Bytes(n,0xa5)}; auto b=encode_gip(p); check((b.size()-n)%2==0,"Odd GIP header"); const auto decoded=parse_gip(b);
                check(decoded.size()==1 && decoded[0].payload==p.payload && decoded[0].chunk_offset==off,"GIP roundtrip mismatch");
            }
        });
        test("Bidirectional GIP chunks and ACK flow",[]{
            std::deque<GipPacket> a_to_b,b_to_a; Bytes result;
            GipLink a(0,[&](const auto &p){a_to_b.push_back(p);},[](const auto &){}),b(0,[&](const auto &p){b_to_a.push_back(p);},[&](const auto &p){result=p.payload;});
            Bytes data(768); for(size_t i=0;i<data.size();++i)data[i]=static_cast<uint8_t>(i); a.send(6,data,true);
            for(unsigned i=0;i<100 && (!a_to_b.empty() || !b_to_a.empty());++i) { while(!a_to_b.empty()){auto p=a_to_b.front();a_to_b.pop_front();b.receive(p);}while(!b_to_a.empty()){auto p=b_to_a.front();b_to_a.pop_front();a.receive(p);} }
            check(result==data,"Chunk exchange did not complete"); a.send(6,data,true);
        });
        test("Reject missing, conflicting, oversized chunks",[]{
            GipLink l(0,[](const auto &){},[](const auto &){});
            rejects([&]{l.receive({6,0xa0,1,0,{1}});}); rejects([&]{l.receive({6,0xe0,1,65536,{1}});});
            l.receive({6,0xe0,1,10,{1,2}}); rejects([&]{l.receive({6,0xa0,1,10,{}});});
        });
        test("P_SHA256 independent Python vector",[]{
            Bytes key(48),seed(64); for(uint8_t i=0;i<48;++i)key[i]=i;for(uint8_t i=0;i<64;++i)seed[i]=i;
            check(hex(auth_prf("Master Secret",key,seed,48))=="e7417868f8f4c33496130e45c0858851db122daef239b9be25739322361a1447e68df9a564e526cefad5a910592d5c46","PRF vector mismatch");
        });
        test("P256 endian interoperability vector",[]{
            auto g=unhex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
            auto peer=unhex("7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc4766997807775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1");
            Bytes pub; check(hex(auth_ecdh(peer,pub,scalar_blob(1,g)))=="23775201799b2234a18e8071e409cec80d42632fe77534180afdc533c9b76f81" && pub==g,"ECDH vector mismatch");
            rejects([&]{auth_ecdh(Bytes(64),pub);});
        });
        test("Auth v1 RSA full handshake",[]{handshake(false,false);});
        test("Auth v2 ECDH upgrade and full handshake",[]{handshake(true,false);});
        test("Empty version 3 advertisement still requires complete v2 authentication",[]{handshake(true,false,true);});
        test("Unsupported auth version cannot loop, install keys or complete",[]{
            Bytes sent,key;Auth auth([&](auto b,bool){sent.assign(b.begin(),b.end());},[&](auto b){key.assign(b.begin(),b.end());});
            const Bytes ack{0,0xc1,0,1,0,0};auth.start();auth.receive(ack);
            auto hello=auth_response(2,{});hello[7]=3;auth.receive(hello);
            check(sent[3]==0x21 && auth.version()==2 && auth.peer_version()==3 && !auth.complete() && key.empty(),"Version probe accepted empty hello as authentication");
            auth.receive(ack);const auto before=sent;auto reply=auth_response(0x22,{});reply[7]=3;
            bool unsupported=false;try{auth.receive(reply);}catch(const UnsupportedAuthVersion &){unsupported=true;}
            check(unsupported && sent==before && key.empty() && !auth.complete(),"Unsupported version restarted handshake or installed a key");
            auth.start();auth.receive(ack);hello=auth_response(2,Bytes(80));hello[7]=3;
            rejects([&]{auth.receive(hello);});check(auth.version()==1 && key.empty(),"Unexpected version with data accepted");
        });
        test("Invalid device transcript installs no key",[]{handshake(true,true);});
        test("WLAN QoS layout and encrypted flag",[]{
            const Bytes mac{1,2,3,4,5,6},host{6,5,4,3,2,1},gip{4,32,1,0};
            const auto plain=wlan_packet(2,mac,host,false,gip),encrypted=wlan_packet(2,mac,host,true,gip);
            check(plain.size()==68 && plain[7]==1 && plain[17]==1,"WCID framing mismatch");
            check(plain[32]==0x88 && plain[33]==2 && encrypted[33]==0x42,"QoS control mismatch");
            check(plain[18]==30 && plain[19]==0 && std::equal(gip.begin(),gip.end(),plain.begin()+60),"TXWI length/padding mismatch");
            const auto pcm=wlan_packet(2,mac,host,true,Bytes(1542),true);
            check(pcm[6]==2 && pcm[7]==1 && pcm[33]==0x42 && (pcm[18]|(size_t(pcm[19])<<8))==1568,"Audio queue/length incorrect");
        });
        test("Identification bounds and classes",[]{
            Bytes b(33); b[26]=16; b[32]=1; const std::string name="Windows.Xbox.Input.Gamepad"; b.push_back(static_cast<uint8_t>(name.size())); b.push_back(0); b.insert(b.end(),name.begin(),name.end());
            check(identification_classes(b)==std::vector<std::string>{name},"Class parse mismatch"); b[33]=255; rejects([&]{identification_classes(b);});
            b[33]=static_cast<uint8_t>(name.size()+1); b.push_back(0); check(identification_classes(b)==std::vector<std::string>{name},"NUL class parse mismatch");
        });
        test("Share and DLI interface GUIDs require valid identification bounds",[]{
            const auto share=unhex("fed2ddec87d39442bd961a712e3dc77d"),dli=unhex("6be5f287bbc3b1498265fffff37799ee");
            Bytes b(33);b[28]=16;b[32]=2;b.insert(b.end(),share.begin(),share.end());b.insert(b.end(),dli.begin(),dli.end());
            const auto caps=identification_capabilities(b);check(caps.share && caps.dli,"Series GUIDs missing");
            b[33]^=1;check(!identification_capabilities(b).share && identification_capabilities(b).dli,"Unknown GUID accepted as Share");
            b.pop_back();rejects([&]{identification_capabilities(b);});
            b[28]=255;rejects([&]{identification_capabilities(b);});
            b[28]=0;check(!identification_capabilities(b).share,"Absent interfaces must not imply Share");
        });
        test("Share position follows packet tail and optional DLI timestamps",[]{
            for(const bool dli:{false,true})for(const size_t extra:{0u,8u}){
                Bytes b(32+(dli?8:0)+extra);b[0]=16;
                const auto share_at=b.size()-18-(dli?8:0);b[share_at]=1;
                auto p=parse_gamepad(b,{true,dli});check(p.buttons==16 && p.share,"Share offset incorrect");
                b[share_at]=0;check(!parse_gamepad(b,{true,dli}).share,"Share release missing");
                b[share_at]=1;check(!parse_gamepad(b).share,"Non-Series input inferred Share from size");
                b.resize(14+(dli?26:18)-1);rejects([&]{parse_gamepad(b,{true,dli});});
            }
        });
        test("Battery status preserves all source/level values and connection flag",[]{
            for(uint8_t type=0;type<4;++type)for(uint8_t level=0;level<4;++level){
                Bytes b{static_cast<uint8_t>(0x80|(type<<2)|level),0,0,0,0};const auto status=parse_battery_status(b);
                check(status.connected && status.type==type && status.level==level,"Battery bits incorrect");
                b[0]&=0x7f;check(!parse_battery_status(b).connected,"Disconnected status lost");
            }
            rejects([]{parse_battery_status(Bytes(3));});
        });
        test("GIP PCM formats and bounded identification format pairs",[]{
            check(audio_format(5).rate==16000 && audio_format(5).interval_bytes==256,"16k mono format incorrect");
            check(audio_format(9).rate==24000 && audio_format(9).interval_bytes==384,"24k mono format incorrect");
            check(audio_format(16).channels==2 && audio_format(16).interval_bytes==1536,"48k stereo format incorrect");
            rejects([]{audio_format(0);});
            Bytes b(37);b[20]=16;b[32]=2;b[33]=5;b[34]=16;b[35]=9;b[36]=16;
            check(identification_audio_formats(b)==std::vector<std::array<uint8_t,2>>{{5,16},{9,16}},"Audio pairs incorrect");
            b.pop_back();rejects([&]{identification_audio_formats(b);});b[20]=255;rejects([&]{identification_audio_formats(b);});b[20]=0;check(identification_audio_formats(b).empty(),"Absent audio formats must stay absent");
        });
        test("Headset waits for format acceptance before power and authentication",[]{
            using Clock=HeadsetAudio::Clock;const auto start=Clock::time_point{};std::vector<std::pair<uint8_t,Bytes>> sent;unsigned powers=0;
            HeadsetAudio h(5,16,start,[&](uint8_t c,std::span<const uint8_t> b){sent.emplace_back(c,Bytes(b.begin(),b.end()));},[&]{++powers;});
            h.tick(start+std::chrono::milliseconds(999));check(sent.empty(),"Headset configured before required delay");
            h.tick(start+std::chrono::seconds(1));check(sent.size()==1 && sent.back()==std::pair<uint8_t,Bytes>{8,{2,5,16}},"Wrong format proposal");
            h.receive(Bytes{2,5,16},start+std::chrono::seconds(1));h.tick(start+std::chrono::milliseconds(1999));check(powers==0,"Power started before format delay");
            h.tick(start+std::chrono::seconds(2));check(powers==1 && sent.back()==std::pair<uint8_t,Bytes>{5,{0}},"Power/auth missing");
            check(h.stage()==HeadsetAudio::Stage::waiting_volume,"Streaming before initial volume");
            h.receive(Bytes{3,4,80,50,70,0,0,0},start+std::chrono::seconds(2));check(h.stage()==HeadsetAudio::Stage::streaming,"Volume did not start streaming");
            h.receive(Bytes{2,5,16},start+std::chrono::seconds(3));h.tick(start+std::chrono::seconds(10));check(powers==1,"Duplicate format restarted authentication");
        });
        test("Headset counter-proposal and timeouts fail without powering",[]{
            using Clock=HeadsetAudio::Clock;const auto start=Clock::time_point{};unsigned powers=0;std::vector<Bytes> sent;
            HeadsetAudio h(5,16,start,[&](uint8_t,std::span<const uint8_t> b){sent.emplace_back(b.begin(),b.end());},[&]{++powers;});
            h.tick(start+std::chrono::seconds(1));h.receive(Bytes{2,9,16},start+std::chrono::seconds(2));
            check(sent.back()==Bytes({2,9,16}) && h.input().rate==24000,"Counter-proposal not accepted");
            h.tick(start+std::chrono::seconds(7));check(h.stage()==HeadsetAudio::Stage::failed && powers==0,"Negotiation timeout powered headset");
            rejects([&]{h.receive(Bytes{2,9},start);});rejects([&]{h.receive(Bytes{3,4},start);});
            HeadsetAudio unsupported(5,16,start,[](uint8_t,std::span<const uint8_t>){},[&]{++powers;});unsupported.tick(start+std::chrono::seconds(1));
            unsupported.receive(Bytes{2,255,16},start+std::chrono::seconds(1));check(unsupported.stage()==HeadsetAudio::Stage::failed && powers==0,"Unsupported format accepted");
        });
        test("Audio IPC preserves PCM and rejects oversized messages",[]{
            Bytes pcm(1536);pcm.front()=123;pcm.back()=231;
            const auto message=audio_message(2,pcm);const auto payload=audio_payload(message);
            check(message[0]==2 && payload.size()==1536 && std::equal(payload.begin(),payload.end(),pcm.begin()),"PCM changed in IPC");
            auto malformed=message;malformed[2]=255;malformed[3]=255;rejects([&]{audio_payload(malformed);});
            rejects([]{audio_message(1,Bytes(audio_message_size));});
            check(audio_payload(audio_message(4)).empty(),"Quit message contains audio");
        });
        test("Synthetic microphone blocks preserve every PCM sample and header padding",[]{
            Bytes pcm(384);for(size_t n=0;n<pcm.size();++n)pcm[n]=static_cast<uint8_t>(n);
            const auto blocks=synthetic_microphone_blocks(pcm);
            Bytes reconstructed;
            for(const auto &block:blocks){
                check(block.size()==98 && block[96]==0 && block[97]==0,"Invalid native GIP microphone padding");
                // SendAudio adds a two-byte prefix but declares argument size as payload size.
                Bytes declared{0,0};declared.insert(declared.end(),block.begin(),block.end());declared.resize(block.size());
                reconstructed.insert(reconstructed.end(),declared.begin()+2,declared.end());
            }
            check(reconstructed==pcm,"Native microphone framing drops or duplicates PCM samples");
            rejects([]{synthetic_microphone_blocks(Bytes(383));});rejects([]{synthetic_microphone_blocks(Bytes(386));});
        });
        test("GIP to XInput button mapping, axes and trigger limits",[]{
            constexpr uint16_t buttons[]={0x10,0x20,0x1000,0x2000,0x4000,0x8000,1,2,4,8,0x100,0x200,0x40,0x80};
            for(unsigned i=0;i<14;++i){GamepadInput p{};p.buttons=static_cast<uint16_t>(1u<<(i+2));check(virtual_report(p).buttons==buttons[i],"Wrong XInput button mapping");}
            GamepadInput p{0xffff,1023,65535,-32768,32767,-1234,5678};const auto r=virtual_report(p,true);
            check(r.buttons==0xf7ff && r.left_trigger==255 && r.right_trigger==255 && r.left_y==32767 && r.right_y==5678,"Range/polarity mismatch");
            p.left_trigger=512;p.right_trigger=0;check(virtual_report(p).left_trigger==128 && virtual_report(p).right_trigger==0,"Trigger rounding mismatch");
        });
        test("XInput rumble scaling and non-internal GIP command",[]{
            const auto p=rumble_packet(255,128);check(p==std::array<uint8_t,9>{0,15,0,0,100,50,255,0,235},"Rumble wire mismatch");
            const auto stopped=rumble_packet(0,0);check(stopped[4]==0 && stopped[5]==0,"Rumble stop failed");
            GipPacket sent{};GipLink link(0,[&](const auto &b){sent=b;},[](const auto &){});link.send(9,p,false,false);
            check(sent.command==9 && sent.options==0 && sent.payload.size()==9,"Rumble incorrectly marked internal");
        });
        test("Native GIP trigger ABI and signed axes",[]{
            GamepadInput p{0xffff,1023,512,-32768,32767,1234,-5678};const auto r=native_input_report(p);
            check(r[0]==0xfc && r[1]==255 && r[2]==255 && r[3]==255,"Native buttons/full trigger mismatch");
            const unsigned half=r[4]|(unsigned(r[5])<<8);check((half>>6)==512 && r[6]==0 && r[7]==128 && r[8]==255 && r[9]==127,"Native ABI scaling/polarity mismatch");
            p.left_trigger=65535;check(native_input_report(p)[3]==255,"Native trigger must clamp");
        });
        test("Native four-motor feedback preserves wire mask and timings",[]{
            const Bytes r{15,75,100,25,50,246,9,0,0,235};const auto p=native_rumble_report(r);
            check(p && *p==std::array<uint8_t,9>{0,15,75,100,25,50,255,0,235},"Native feedback mismatch");
            auto bad=r;bad[1]=101;check(!native_rumble_report(bad),"Invalid motor accepted");
            check(!native_rumble_report(std::span(r).first(9)),"Truncated feedback accepted");
            bad=r;bad[0]=16;check(!native_rumble_report(bad),"Invalid motor mask accepted");
            bad=r;bad[5]=11;bad[6]=0;bad[7]=1;const auto short_time=native_rumble_report(bad);check(short_time && (*short_time)[6]==2 && (*short_time)[7]==1,"Duration must round up to 10ms units");
        });
        test("Pairing scans empty channels and freezes while a client connects",[]{
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int {if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int {if(actual)*actual=n;return 0;};
            constexpr uint8_t channels[]={1,6,11,36,40,44,48,149,153,157,161,165};
            for(size_t n=0;n<XONE_MT_NUM_CHANNELS;++n)mt.channels[n].index=channels[n];mt.channel=&mt.channels[11];
            Session session(mt);RadioEvent e;e.kind=RadioEvent::Kind::button;session.process(e);
            const auto start=std::chrono::steady_clock::now();check(session.pairing_active(),"Button did not enable pairing");
            session.tick(start+std::chrono::milliseconds(1900));check(mt.channel->index==165,"Channel changed before scan interval");
            session.tick(start+std::chrono::milliseconds(2100));check(mt.channel->index==1,"Channel scan did not wrap");
            e.kind=RadioEvent::Kind::association;e.address={2,2,3,4,5,6};session.process(e);
            session.tick(start+std::chrono::seconds(5));check(mt.channel->index==1,"Channel changed during client handshake");
            e.kind=RadioEvent::Kind::lost;e.wcid=1;session.process(e);session.tick(start+std::chrono::seconds(7));check(mt.channel->index==6,"Empty channel scan did not resume");
            session.tick(start+std::chrono::seconds(40));check(!session.pairing_active() && mt.channel->index==6,"Pairing timeout scanned another channel");session.close();
        });
        test("Idle reconnect scan never enables pairing and freezes on association",[]{
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int {if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int {if(actual)*actual=n;return 0;};
            constexpr uint8_t channels[]={1,6,11,36,40,44,48,149,153,157,161,165};
            for(size_t n=0;n<XONE_MT_NUM_CHANNELS;++n)mt.channels[n].index=channels[n];mt.channel=&mt.channels[11];
            Session session(mt);const auto start=std::chrono::steady_clock::now();
            session.tick(start+std::chrono::seconds(3));check(mt.channel->index==165,"Idle scan enabled by default");
            session.enable_idle_channel_scan();
            for(size_t n=0;n<XONE_MT_NUM_CHANNELS;++n){
                session.tick(start+std::chrono::seconds(5+3*n));
                check(mt.channel->index==channels[n] && !session.pairing_active(),"Reconnect scan skipped channel or enabled pairing");
            }
            RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={2,2,3,4,5,6};session.process(e);
            session.tick(start+std::chrono::seconds(45));check(mt.channel->index==165,"Reconnect scan interrupted a client");
            e.kind=RadioEvent::Kind::client_command;e.command=1;session.process(e);
            check(session.status().size()==1 && !session.pairing_active(),"Reconnect scan accepted a new pair request");
            e.kind=RadioEvent::Kind::lost;e.wcid=1;session.process(e);session.tick(start+std::chrono::seconds(48));
            check(mt.channel->index==1 && !session.pairing_active(),"Reconnect scan did not resume after disconnect");session.close();
        });
        test("Headset PC opt-out requires exact model and GUID, never installs encryption",[]{
            for(unsigned scenario=0;scenario<3;++scenario){
                struct Output final:AudioOutput{
                    unsigned connections=0,captures=0,playbacks=0;
                    void connect(uint8_t,uint8_t)override{++connections;}
                    void disconnect(uint8_t)override{}
                    void capture(uint8_t,uint8_t,std::span<const uint8_t> b)override{check(b.size()==384,"Wrong opt-out microphone size");++captures;}
                    Bytes playback(uint8_t,uint8_t)override{++playbacks;return Bytes(1536);}
                    AudioEndpoints endpoints(uint8_t,uint8_t)override{return {};}
                } output;
                usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;mt.channel=&mt.channels[0];mt.channel->index=165;
                usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
                usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int{if(actual)*actual=n;return 0;};
                Session session(mt,nullptr,false,&output);RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={1,2,3,4,5,6};session.process(e);
                e.kind=RadioEvent::Kind::gip;e.wcid=1;Bytes announce(28);announce[8]=0x5e;announce[9]=4;announce[10]=scenario==1?0x31:0x32;announce[11]=0x0b;
                auto deliver=[&](uint8_t command,Bytes payload){e.kind=RadioEvent::Kind::gip;e.payload=encode_gip({command,32,1,0,std::move(payload)});session.process(e);};
                deliver(2,announce);
                Bytes identify(33);identify[20]=16;identify[32]=1;identify.push_back(9);identify.push_back(16);
                identify[26]=19;identify.push_back(1);const std::string name="Windows.Xbox.Input.Headset";
                identify.push_back(static_cast<uint8_t>(name.size()));identify.push_back(0);identify.insert(identify.end(),name.begin(),name.end());
                const auto relative=identify.size()-16;identify[28]=static_cast<uint8_t>(relative);identify[29]=static_cast<uint8_t>(relative>>8);identify.push_back(1);
                auto guid=unhex("77ce347ae27dc6458ca40042c08bd94a");if(scenario==2)guid.back()^=1;identify.insert(identify.end(),guid.begin(),guid.end());deliver(4,identify);
                const auto identified_revision=session.status_revision();deliver(4,identify);
                check(session.status_revision()==identified_revision,"Duplicate identification bypassed status cadence");
                const auto now=std::chrono::steady_clock::now();session.tick(now+std::chrono::seconds(2));
                if(!scenario){
                    const auto proposed=session.status_revision();deliver(8,{2,5,16});
                    check(session.status_revision()>proposed && session.status()[0].audio->input_rate==16000,"Changed format without stage change did not wake publication");
                    const auto revised=session.status_revision();deliver(8,{2,9,16});
                    check(session.status_revision()>revised && session.status()[0].audio->input_rate==24000,"Second audio format change did not wake publication");
                }
                const auto format_sent=session.status_revision();deliver(8,{2,9,16});
                check(session.status_revision()>format_sent,"Audio format acceptance did not wake publication");
                const auto accepted=session.status_revision();deliver(8,{2,9,16});
                check(session.status_revision()==accepted,"Duplicate audio format bypassed status cadence");
                session.tick(now+std::chrono::seconds(2));
                const auto waiting_volume=session.status_revision();deliver(8,{3,4,80,50,70,0,0,0});
                check(session.status_revision()>waiting_volume,"Audio streaming transition did not wake publication");
                const auto streaming=session.status_revision();
                for(unsigned i=0;i<1000;++i)deliver(8,{3,4,80,50,70,0,0,0});
                check(session.status_revision()==streaming,"Repeated audio volume bypassed status cadence");
                session.tick(now+std::chrono::seconds(3));
                const auto status=session.status();check(status.size()==1 && !status[0].encrypted && session.authenticated()==0,"Opt-out fabricated authentication/key");
                if(!scenario){
                    check(status[0].authentication=="pc-opt-out" && output.connections==1 && output.playbacks==1 && session.audio_active(),"Exact model/GUID could not stream plaintext audio");
                    deliver(0x60,Bytes(386));check(output.captures==1,"Plaintext microphone not forwarded");
                    e.kind=RadioEvent::Kind::client_command;e.command=16;session.process(e);check(!session.status()[0].encrypted && session.authenticated()==0,"Opt-out accepted CCMP without a key");
                }else check(status[0].authentication=="negotiating" && !output.connections && !session.audio_active(),"Different model/GUID bypassed authentication");
                session.close();
            }
        });
        test("Status revision ignores repeated controls and wakes on battery changes",[]{
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;mt.channel=&mt.channels[0];mt.channel->index=165;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int{if(actual)*actual=n;return 0;};
            Session session(mt,nullptr,false);const auto initial=session.status_revision();
            RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={2,0,0,0,0,1};session.process(e);
            const auto connected=session.status_revision();check(connected>initial,"Connection did not wake status publication");
            e.kind=RadioEvent::Kind::gip;e.wcid=1;e.payload=encode_gip({0x60,32,1,0,Bytes(386)});
            for(unsigned i=0;i<1000;++i)session.process(e);
            check(session.status_revision()==connected,"PCM counters bypassed status cadence");
            e.payload=encode_gip({0x7f,32,1,0,{}});
            for(unsigned i=0;i<1000;++i)session.process(e);
            check(session.status_revision()==connected,"Ignored commands bypassed status cadence");
            e.payload=encode_gip({3,33,1,0,{0x81,0,0,0}});session.process(e);
            check(session.status_revision()==connected,"Accessory battery bypassed parent status cadence");
            e.payload=encode_gip({7,32,1,0,{0,0x5b}});session.process(e);
            check(session.status_revision()==connected,"Guide event absent from JSON bypassed status cadence");
            e.payload=encode_gip({3,32,1,0,{0x81,0,0,0}});session.process(e);
            const auto battery=session.status_revision();check(battery>connected,"First battery status did not wake publication");
            for(unsigned i=0;i<1000;++i)session.process(e);
            check(session.status_revision()==battery,"Unchanged battery status bypassed status cadence");
            e.payload=encode_gip({3,32,1,0,{0x82,0,0,0}});session.process(e);
            const auto level=session.status_revision();check(level>battery && session.status()[0].battery->level==2,"Battery level change did not wake publication");
            e.payload=encode_gip({3,32,1,0,{0x86,0,0,0}});session.process(e);
            check(session.status_revision()>level && session.status()[0].battery->type==1,"Battery type change did not wake publication");
            const auto control=session.status_revision();e.kind=RadioEvent::Kind::lost;session.process(e);
            check(session.status_revision()>control,"Disconnect did not wake status publication");
            session.close();
        });
        test("Explicit re-pair resets only the requesting radio session",[]{
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;mt.channel=&mt.channels[0];mt.channel->index=165;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int{if(actual)*actual=n;return 0;};
            Session session(mt);RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={1,2,3,4,5,6};session.process(e);
            const auto target=e.address;e.address={6,5,4,3,2,1};session.process(e);
            e.kind=RadioEvent::Kind::client_command;e.command=1;e.address=target;session.process(e);
            check(session.status().size()==2,"Pair request outside pairing removed a session");
            e.kind=RadioEvent::Kind::button;session.process(e);e.kind=RadioEvent::Kind::client_command;session.process(e);
            auto status=session.status();check(status.size()==1 && status[0].wcid==2 && !session.pairing_active(),"Re-pair did not isolate and reset the requesting client");
            e.kind=RadioEvent::Kind::association;session.process(e);status=session.status();
            check(status.size()==2 && status[0].wcid==1 && status[0].authentication=="not-started" && !status[0].encrypted,"Fresh association inherited old session state");
            session.close();
        });
        test("Raw USB GIP owns handshake and separates audio from interrupt",[]{
            struct Raw final:RawGipOutput{
                struct Event{uint8_t kind,wcid;Bytes data;};
                std::vector<Event> events;std::vector<RawGipTransmit> commands;std::vector<int> results;
                bool target(std::span<const uint8_t> b)const override{return b.size()==6 && b[0]==1;}
                void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> b)override{events.push_back({kind,wcid,Bytes(b.begin(),b.end())});}
                std::vector<RawGipTransmit> poll()override{auto out=std::move(commands);commands.clear();return out;}
                void complete(uint8_t,uint32_t,int status)override{results.push_back(status);}
            } raw;
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;mt.channel=&mt.channels[0];mt.channel->index=165;
            size_t writes=0;usb.context=&writes;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *ctx,u8,void *,int n,int *actual,unsigned)->int{++*static_cast<size_t *>(ctx);if(actual)*actual=n;return 0;};
            Session session(mt,nullptr,false,nullptr,&raw);RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={1,2,3,4,5,6};session.process(e);
            check(raw.events.size()==1 && raw.events[0].kind==1 && raw.events[0].wcid==1,"Raw association missing");
            const auto before=writes;session.tick(std::chrono::steady_clock::now()+std::chrono::seconds(2));
            check(writes==before,"Raw client got competing identification retry");
            e.kind=RadioEvent::Kind::gip;e.wcid=1;e.payload=encode_gip({2,48,1,0,Bytes(28)});session.process(e);
            check(writes==before && raw.events.back().kind==2 && raw.events.back().data==e.payload,"Raw announce rewritten or locally acknowledged");
            e.payload=encode_gip({0x60,32,2,0,Bytes(386)});session.process(e);check(raw.events.back().kind==4 && raw.events.back().data==e.payload,"Radio PCM not isolated from USB interrupt");
            raw.commands={{1,1,encode_gip({4,32,1,0,{}})},{2,2,encode_gip({4,32,1,0,{}})},{1,3,{4}},{1,4,encode_gip({0x60,32,1,0,{}})},
                          {1,5,encode_gip({0x60,32,1,0,Bytes(1536)}),true},{1,6,encode_gip({0x60,32,1,0,Bytes(1535)}),true}};
            session.tick();check(raw.results==std::vector<int>({0,-19,-71,-95,0,-95}),"Raw TX acknowledgement or routing validation wrong");
            e.kind=RadioEvent::Kind::client_command;e.command=16;session.process(e);check(!session.status()[0].encrypted,"Raw mode enabled encryption without a key");
            e.kind=RadioEvent::Kind::lost;session.process(e);check(raw.events.back().kind==3 && session.status().empty(),"Raw removal not forwarded");
            session.close();
        });
        test("Malformed radio GIP is isolated per WCID before raw or fallback dispatch",[]{
            for(const bool passthrough:{false,true}){
                struct Raw final:RawGipOutput{
                    bool enabled;std::vector<std::pair<uint8_t,Bytes>> packets;
                    explicit Raw(bool value):enabled(value){}
                    bool target(std::span<const uint8_t>)const override{return enabled;}
                    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data)override{if(kind==2 || kind==4)packets.emplace_back(wcid,Bytes(data.begin(),data.end()));}
                    std::vector<RawGipTransmit> poll()override{return {};}
                    void complete(uint8_t,uint32_t,int)override{}
                } raw(passthrough);
                usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;
                usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
                usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int{if(actual)*actual=n;return 0;};
                Session session(mt,nullptr,false,nullptr,&raw);RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={2,0,0,0,0,1};session.process(e);
                e.address={2,0,0,0,0,2};session.process(e);e.kind=RadioEvent::Kind::gip;e.wcid=1;
                const auto valid=encode_gip({2,32,1,0,Bytes(28)});
                for(const auto &bad:std::vector<Bytes>{{2},{2,32,1,128,128,128,128},{2,32,1,4,0},{2,128,1,0}}){
                    e.payload=bad;session.process(e);
                }
                // A valid prefix must not escape when a later frame is malformed.
                e.payload=valid;e.payload.push_back(2);session.process(e);
                check(raw.packets.empty(),"Partially parsed transfer was delivered");
                auto status=session.status();check(status.size()==2 && status[0].malformed_gip_packets==5 && status[1].malformed_gip_packets==0,
                    "Parse failure removed a device or affected the sibling counter");
                e.wcid=2;e.payload=valid;session.process(e);e.wcid=1;session.process(e);
                if(passthrough)check(raw.packets.size()==2 && raw.packets[0].first==2 && raw.packets[1].first==1,"A malformed peer blocked later input or its sibling");
                e.wcid=0;e.payload={2};session.process(e);e.wcid=17;session.process(e);
                check(session.status()[0].malformed_gip_packets==5,"Unknown WCID changed an active counter");
                e.kind=RadioEvent::Kind::lost;e.wcid=1;session.process(e);
                check(session.status().size()==1 && session.status()[0].wcid==2,"Peer removal affected sibling");
                e.kind=RadioEvent::Kind::association;e.address={2,0,0,0,0,1};session.process(e);
                check(session.status()[0].malformed_gip_packets==0,"Reassociation inherited malformed packet count");
                session.close();
            }
        });
        test("GIP isolation does not suppress output or USB transport failures",[]{
            struct Raw final:RawGipOutput{
                bool fail=false;std::vector<RawGipTransmit> tx;
                bool target(std::span<const uint8_t>)const override{return true;}
                void event(uint8_t kind,uint8_t,std::span<const uint8_t>)override{if(kind==2 && fail)throw std::runtime_error("Output failure");}
                std::vector<RawGipTransmit> poll()override{auto out=std::move(tx);tx.clear();return out;}
                void complete(uint8_t,uint32_t,int)override{}
            } raw;
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int{if(actual)*actual=n;return 0;};
            Session session(mt,nullptr,false,nullptr,&raw);RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={2,0,0,0,0,1};session.process(e);
            raw.fail=true;e.kind=RadioEvent::Kind::gip;e.wcid=1;e.payload=encode_gip({4,32,1,0,{}});
            bool thrown=false;try{session.process(e);}catch(const std::runtime_error &){thrown=true;}
            check(thrown && !session.status()[0].malformed_gip_packets,"Output failure was hidden as malformed GIP");
            raw.fail=false;raw.tx={{1,1,e.payload}};usb.bulk=[](void *,u8,void *,int,int *,unsigned)->int{return -EREMOTEIO;};
            thrown=false;try{session.tick();}catch(const std::runtime_error &){thrown=true;}
            check(thrown && usb.sticky_error==-EREMOTEIO,"Physical USB failure did not propagate to recovery");
        });
        test("USB brokers isolate WCIDs, equal completion tokens and reassociation",[]{
            struct Raw final:RawGipOutput{
                uint8_t id;unsigned events=0;
                std::vector<RawGipTransmit> requests;
                std::vector<std::pair<uint32_t,int>> results;
                explicit Raw(uint8_t n):id(n){}
                bool target(std::span<const uint8_t> b)const override{return b.size()==6 && b[0]==id;}
                void event(uint8_t,uint8_t,std::span<const uint8_t>)override{++events;}
                std::vector<RawGipTransmit> poll()override{auto out=std::move(requests);requests.clear();return out;}
                void complete(uint8_t,uint32_t token,int status)override{results.emplace_back(token,status);}
            };
            auto first=std::make_unique<Raw>(uint8_t(1)),second=std::make_unique<Raw>(uint8_t(2));auto *a=first.get();auto *b=second.get();
            std::vector<std::unique_ptr<RawGipOutput>> peers;peers.push_back(std::move(first));peers.push_back(std::move(second));RawGipRouter router(std::move(peers));
            const Bytes mac_a{1,2,3,4,5,6},mac_b{2,2,3,4,5,6};router.event(1,3,mac_a);router.event(1,1,mac_b);
            router.event(4,3,Bytes(386));check(a->events==2 && b->events==1,"Audio crossed USB brokers");
            a->requests={{1,9,{}},{3,7,{}}};b->requests={{1,7,{}}};const auto requests=router.poll();
            check(requests.size()==2 && requests[0].wcid==3 && requests[1].wcid==1 && a->results==std::vector<std::pair<uint32_t,int>>{{9,-19}},"Foreign WCID request accepted");
            router.complete(3,7,0);router.complete(1,7,-71);check(a->results.back()==std::pair<uint32_t,int>{7,0} && b->results.back()==std::pair<uint32_t,int>{7,-71},"Equal completion tokens crossed brokers");
            router.event(3,3);router.event(3,1);router.event(1,3,mac_b);a->requests={{3,11,{}}};
            check(router.poll().empty() && a->results.back()==std::pair<uint32_t,int>{11,-19},"Stale broker transmitted to reused WCID");
            rejects([&]{router.event(1,17,mac_a);});rejects([&]{router.event(1,3,mac_a);});
        });
        test("USB router interleaves busy peers and rotates the first device",[]{
            struct Raw final:RawGipOutput{
                uint8_t id;
                std::vector<RawGipTransmit> requests;
                explicit Raw(uint8_t n):id(n){}
                bool target(std::span<const uint8_t> b)const override{return b.size()==6 && b[0]==id;}
                void event(uint8_t,uint8_t,std::span<const uint8_t>)override{}
                std::vector<RawGipTransmit> poll()override{auto out=std::move(requests);requests.clear();return out;}
                void complete(uint8_t,uint32_t,int)override{throw std::runtime_error("Unexpected rejected request");}
            };
            std::vector<std::unique_ptr<RawGipOutput>> peers;
            std::array<Raw *,3> raw{};
            for(uint8_t n=1;n<=3;++n){auto p=std::make_unique<Raw>(n);raw[n-1]=p.get();peers.push_back(std::move(p));}
            RawGipRouter router(std::move(peers));
            for(uint8_t n=1;n<=3;++n)router.event(1,n,Bytes{n,2,3,4,5,6});
            for(uint32_t token=1;token<=64;++token)raw[0]->requests.push_back({1,token,{},true});
            raw[1]->requests={{2,101,{},true},{2,102,{},true}};
            raw[2]->requests={{3,201,{}}};
            const auto first=router.poll();
            check(first.size()==67 && first[0].token==1 && first[1].token==101 && first[2].token==201 &&
                  first[3].token==2 && first[4].token==102,"Busy peer starved headset/controller");
            uint32_t last=0;for(const auto &r:first)if(r.wcid==1){check(r.token==last+1,"Peer FIFO order changed");last=r.token;}
            for(uint8_t start=2;start<=3;++start){
                for(uint8_t n=1;n<=3;++n)raw[n-1]->requests={{n,n,{}}};
                const auto round=router.poll();
                check(round.size()==3 && round[0].wcid==start,"First peer did not rotate");
            }
            check(router.poll().empty(),"Empty peers returned requests");
        });
        test("Raw USB sibling preserves the fallback headset audio output",[]{
            struct Raw final:RawGipOutput{
                unsigned events=0;
                bool target(std::span<const uint8_t> b)const override{return b.size()==6 && b[0]==1;}
                void event(uint8_t,uint8_t,std::span<const uint8_t>)override{++events;}
                std::vector<RawGipTransmit> poll()override{return {};}
                void complete(uint8_t,uint32_t,int)override{}
            } raw;
            struct Audio final:AudioOutput{
                unsigned connects=0,captures=0,playbacks=0;
                void connect(uint8_t w,uint8_t c)override{check(w==2 && c==0,"Fallback audio attached to raw sibling");++connects;}
                void disconnect(uint8_t)override{}
                void capture(uint8_t w,uint8_t c,std::span<const uint8_t> b)override{check(w==2 && c==0 && b.size()==384,"Fallback capture crossed devices");++captures;}
                Bytes playback(uint8_t w,uint8_t c)override{check(w==2 && c==0,"Fallback playback crossed devices");++playbacks;return Bytes(1536);}
                AudioEndpoints endpoints(uint8_t,uint8_t)override{return {};}
            } audio;
            usb_device usb{};device dev{};xone_mt76 mt{};mt.dev=&dev;mt.udev=&usb;mt.channel=&mt.channels[0];mt.channel->index=165;
            usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int{if(type&128)std::memset(data,0,n);return n;};
            usb.bulk=[](void *,u8,void *,int n,int *actual,unsigned)->int{if(actual)*actual=n;return 0;};
            Session session(mt,nullptr,false,&audio,&raw);RadioEvent e;e.kind=RadioEvent::Kind::association;e.address={1,2,3,4,5,6};session.process(e);
            e.address={2,2,3,4,5,6};session.process(e);check(raw.events==1,"Non-target association went to raw USB");
            e.wcid=2;
            auto deliver=[&](uint8_t command,Bytes payload){e.kind=RadioEvent::Kind::gip;e.payload=encode_gip({command,32,1,0,std::move(payload)});session.process(e);};
            Bytes announce(28);announce[8]=0x5e;announce[9]=4;announce[10]=0x32;announce[11]=0x0b;deliver(2,announce);
            Bytes identify(33);identify[20]=16;identify[32]=1;identify.push_back(9);identify.push_back(16);identify[26]=19;identify.push_back(1);
            const std::string name="Windows.Xbox.Input.Headset";identify.push_back(static_cast<uint8_t>(name.size()));identify.push_back(0);identify.insert(identify.end(),name.begin(),name.end());
            const auto relative=identify.size()-16;identify[28]=static_cast<uint8_t>(relative);identify[29]=static_cast<uint8_t>(relative>>8);identify.push_back(1);
            const auto guid=unhex("77ce347ae27dc6458ca40042c08bd94a");identify.insert(identify.end(),guid.begin(),guid.end());deliver(4,identify);
            const auto now=std::chrono::steady_clock::now();session.tick(now+std::chrono::seconds(2));deliver(8,{2,9,16});session.tick(now+std::chrono::seconds(2));deliver(8,{3,4,80,50,70,0,0,0});session.tick(now+std::chrono::seconds(3));
            deliver(0x60,Bytes(386));check(audio.connects==1 && audio.playbacks==1 && audio.captures==1 && raw.events==1,"Fallback audio disappeared with a raw USB sibling");
            e.wcid=1;deliver(0x60,Bytes(386));check(raw.events==2 && audio.captures==1,"Raw PCM reached the fallback output");
            e.kind=RadioEvent::Kind::lost;session.process(e);check(session.status().size()==1 && session.status()[0].wcid==2 && session.audio_active(),"Raw disconnect removed fallback audio");
            session.close();
        });
        test("Client association, identification, auth start and disconnect",[]{
            struct Mock {
                std::vector<Bytes> writes;
                usb_device usb{}; device dev{}; xone_mt76 mt{};
                Mock() {
                    usb.context=this;
                    usb.control=[](void *,u8,u8 type,u16,u16,void *data,u16 n,unsigned)->int { if(type&128) std::memset(data,0,n); return n; };
                    usb.bulk=[](void *ctx,u8,void *data,int n,int *actual,unsigned)->int { auto &self=*static_cast<Mock *>(ctx); auto *b=static_cast<uint8_t *>(data); self.writes.emplace_back(b,b+n); if(actual)*actual=n; return 0; };
                    mt.dev=&dev; mt.udev=&usb; mt.channel=&mt.channels[0]; mt.channel->index=165;
                }
                GipPacket last_gip() const {
                    const auto &b=writes.back(); check(b.size()>=64,"Missing WLAN frame"); const size_t n=(b[18]|(size_t(b[19])<<8))-26;
                    return parse_gip(std::span<const uint8_t>(b).subspan(60,n)).at(0);
                }
            } mock;
            Session session(mock.mt); RadioEvent e; e.kind=RadioEvent::Kind::association; e.address={1,2,3,4,5,6}; session.process(e);
            const auto count=mock.writes.size(); session.process(e); check(mock.writes.size()==count,"Duplicate client association");
            const auto now=std::chrono::steady_clock::now();session.tick(now+std::chrono::milliseconds(900));check(mock.writes.size()==count,"Identification retry sent too early");
            session.tick(now+std::chrono::milliseconds(1100));check(mock.last_gip().command==4,"Lost announcement did not trigger identification retry");
            for(unsigned n=2;n<=5;++n)session.tick(now+std::chrono::seconds(n+1));
            const auto retries=mock.writes.size();session.tick(now+std::chrono::seconds(20));check(mock.writes.size()==retries,"Identification retry count is unbounded");
            e.kind=RadioEvent::Kind::gip; e.wcid=1; e.payload=encode_gip({2,32,1,0,Bytes(28)}); session.process(e); check(mock.last_gip().command==4,"Identification not requested");
            const auto before_identification=session.status_revision();
            Bytes identify(33); identify[26]=16; identify[32]=1; const std::string name="Windows.Xbox.Input.Gamepad";
            identify.push_back(static_cast<uint8_t>(name.size()));identify.push_back(0);identify.insert(identify.end(),name.begin(),name.end());
            e.payload=encode_gip({4,32,2,0,identify});session.process(e); const auto auth=mock.last_gip();
            check(auth.command==6 && auth.payload.size()==58 && auth.payload[3]==1,"Gamepad auth not started");
            check(session.status_revision()>before_identification && session.status()[0].authentication=="negotiating","Identification/auth start did not wake publication");
            const auto identified_count=mock.writes.size();session.tick(now+std::chrono::seconds(10));check(mock.writes.size()==identified_count,"Identification retry continued after identification");
            e.payload=encode_gip({3,32,3,0,{0x86,0,0,0}});session.process(e);
            const auto status=session.status();check(status.size()==1 && status[0].battery && status[0].battery->type==1 && status[0].battery->level==2,"Session did not retain battery");
            e.payload=encode_gip({3,33,4,0,{0,0,0,0}});session.process(e);session.tick();
            check(session.status().size()==1,"Accessory status removed parent controller");
            const auto before_auth_ack=session.status_revision();
            e.kind=RadioEvent::Kind::gip;e.payload=encode_gip({6,32,5,0,{0,0xc1,0,1,0,0}});session.process(e);
            check(session.status_revision()==before_auth_ack,"Auth ACK without visible state change bypassed cadence");
            auto unsupported=auth_response(2,{});unsupported[7]=3;e.payload=encode_gip({6,32,6,0,unsupported});session.process(e);
            check(mock.last_gip().payload[3]==0x21,"Session did not send bounded v2 probe");
            const auto negotiated=session.status_revision();check(negotiated>before_auth_ack,"Auth version change did not wake publication");
            e.payload=encode_gip({6,32,7,0,{0,0xc1,0,1,0,0}});session.process(e);
            check(session.status_revision()==negotiated,"Negotiation ACK bypassed status cadence");
            unsupported=auth_response(0x22,{});unsupported[7]=3;e.payload=encode_gip({6,32,8,0,unsupported});session.process(e);
            check(session.status()[0].authentication=="unsupported-version" && session.status()[0].peer_auth_version==3,"Unsupported auth not isolated/reported");
            check(session.status_revision()>negotiated,"Auth failure did not wake publication");
            e.kind=RadioEvent::Kind::client_command;e.command=16;session.process(e);check(session.authenticated()==0 && !session.status()[0].encrypted,"Premature encryption accepted");
            e.kind=RadioEvent::Kind::button;session.process(e);check(session.pairing_active(),"Adapter button stopped after client authentication failure");
            e.kind=RadioEvent::Kind::association;e.address={6,5,4,3,2,1};session.process(e);check(session.status().size()==2,"Another client could not associate after auth failure");
            e.kind=RadioEvent::Kind::gip;e.wcid=2;e.payload=encode_gip({4,32,1,0,identify});session.process(e);
            session.tick(std::chrono::steady_clock::now()+std::chrono::seconds(16));
            check(session.status()[1].authentication=="timeout" && !session.status()[1].encrypted,"Silent authentication did not time out independently");
            e.kind=RadioEvent::Kind::lost;e.wcid=2;session.process(e);
            e.kind=RadioEvent::Kind::gip;e.wcid=1;e.payload=encode_gip({3,32,5,0,{0,0,0,0}});session.process(e);
            check(session.status().size()==1,"Client removed during active dispatch");session.tick();check(session.status().empty(),"Status disconnect not processed on tick");
            e.kind=RadioEvent::Kind::lost;session.process(e);const auto after=mock.writes.size();session.process(e);check(mock.writes.size()==after,"Duplicate removal");session.close();
        });
        std::cout<<passed<<" session tests passed\n"; return 0;
    }catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}

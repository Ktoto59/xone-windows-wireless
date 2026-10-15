// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <avrt.h>
#include "usbip_native_output.hpp"
#include "usbip_native_core.hpp"
#include "latest_snapshot.hpp"
#include "appliance_config.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <atomic>
namespace xone {
namespace {
struct Client {
    SOCKET socket=INVALID_SOCKET;WSAEVENT event=WSA_INVALID_EVENT;
    Bytes input,output;size_t sent=0;unsigned phase=0;bool dead=false;
    usbip::Core::Clock::time_point last=usbip::Core::Clock::now();
    explicit Client(SOCKET s):socket(s){
        event=WSACreateEvent();
        if(event==WSA_INVALID_EVENT || WSAEventSelect(socket,event,FD_READ|FD_WRITE|FD_CLOSE)){if(event!=WSA_INVALID_EVENT)WSACloseEvent(event);closesocket(socket);socket=INVALID_SOCKET;throw usbip::Error("Cannot register USB client");}
        BOOL nodelay=TRUE;setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char *>(&nodelay),sizeof(nodelay));
    }
    ~Client(){closesocket(socket);WSACloseEvent(event);}
    void send(Bytes bytes){if(output.size()-sent+bytes.size()>4*1024*1024)throw usbip::Error("USB response queue overflow");if(sent){output.erase(output.begin(),output.begin()+sent);sent=0;}output.insert(output.end(),bytes.begin(),bytes.end());}
    void flush(){
        while(sent<output.size()){
            const auto n=::send(socket,reinterpret_cast<const char *>(output.data()+sent),static_cast<int>(output.size()-sent),0);
            if(n==SOCKET_ERROR){if(WSAGetLastError()==WSAEWOULDBLOCK)return;dead=true;return;}if(!n){dead=true;return;}sent+=static_cast<size_t>(n);
        }output.clear();sent=0;if(phase==3)dead=true;
    }
    void read(){
        std::array<uint8_t,32768> bytes{};
        {const auto n=recv(socket,reinterpret_cast<char *>(bytes.data()),static_cast<int>(bytes.size()),0);
            if(n==SOCKET_ERROR){if(WSAGetLastError()==WSAEWOULDBLOCK)return;dead=true;return;}if(!n){dead=true;return;}
            if(input.size()+static_cast<size_t>(n)>131072)throw usbip::Error("USB request queue overflow");input.insert(input.end(),bytes.begin(),bytes.begin()+n);last=usbip::Core::Clock::now();
        }
    }
};
Bytes import_header(bool ok){return {1,17,0,3,0,0,0,static_cast<uint8_t>(ok?0:1)};}
}
struct NativeUsbOutput::Impl {
    struct Message {uint8_t kind,wcid;uint32_t token=0;int status=0;Bytes data;usbip::Core::Clock::time_point arrived=usbip::Core::Clock::now();};
    Bytes mac;usbip::Profile profile;usbip::Core core;bool audio,automatic,diagnostics;uint16_t product;uint8_t bound_wcid=0;
    SOCKET listener=INVALID_SOCKET;WSAEVENT accept_event=WSA_INVALID_EVENT;
    HANDLE wake=nullptr,timer=nullptr;std::thread thread;std::mutex mutex;
    bool stopping=false;std::string error;std::deque<Message> inbox;std::vector<RawGipTransmit> tx;
    std::string last_usb_error;uint64_t usb_closed=0;
    std::vector<std::unique_ptr<Client>> clients;Client *imported=nullptr;
    std::unique_ptr<LatestSnapshot> status_writer;
    usbip::Core::Clock::time_point last_status{};bool mmcss=false,peer_error=false;DWORD mmcss_error=0;
    std::atomic_uint64_t address_revision=0;
    std::atomic_bool streaming_audio=false;
    uint64_t association_id=0;
    std::optional<std::tuple<bool,uint8_t,uint64_t,bool,uint64_t,bool,uint64_t,std::string>> published_life;
    Impl(uint16_t port,Bytes address,uint16_t pid,bool with_audio,const std::wstring &status,bool auto_select,bool trace):mac(std::move(address)),profile(usbip::profile(pid,with_audio)),core(profile,with_audio),audio(with_audio),automatic(auto_select),diagnostics(trace),product(pid){
        if(mac.size()!=6 || port<1024)throw usbip::Error("Invalid native USB route");
        static std::atomic_uint64_t epochs{GetTickCount64()*1000};association_id=++epochs;
        if(automatic){char prefix[16]{};std::snprintf(prefix,sizeof(prefix),"XONE%04X",pid);profile.virtual_serial=prefix;constexpr char digits[]="0123456789ABCDEF";for(auto b:mac){profile.virtual_serial+=digits[b>>4];profile.virtual_serial+=digits[b&15];}}
        WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw usbip::Error("Cannot initialize sockets");
        try{
            wake=CreateEventW(nullptr,TRUE,FALSE,nullptr);timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            accept_event=WSACreateEvent();listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if(!wake || !timer || accept_event==WSA_INVALID_EVENT || listener==INVALID_SOCKET)throw usbip::Error("Cannot create native USB server");
            BOOL exclusive=TRUE;if(setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char *>(&exclusive),sizeof(exclusive)))throw usbip::Error("Cannot reserve local USB port");
            sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(port);
            if(bind(listener,reinterpret_cast<sockaddr *>(&local),sizeof(local)) || listen(listener,16) || WSAEventSelect(listener,accept_event,FD_ACCEPT|FD_CLOSE))throw usbip::Error("Cannot bind local USB port");
            if(!status.empty()){
                const auto path=std::filesystem::path(status),temporary=std::filesystem::path(status+L".tmp");
                status_writer=std::make_unique<LatestSnapshot>([path,temporary](const std::string &text){
                    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
                    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!out || !out.write(text.data(),static_cast<std::streamsize>(text.size())))return;}
                    for(unsigned attempt=0;attempt<20;++attempt){if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
                });
            }
            thread=std::thread([this]{run();});
        }catch(...){release();throw;}
    }
    ~Impl(){ {std::lock_guard lock(mutex);stopping=true;}SetEvent(wake);if(thread.joinable())thread.join();clients.clear();release(); }
    void release(){if(listener!=INVALID_SOCKET)closesocket(listener);if(accept_event!=WSA_INVALID_EVENT)WSACloseEvent(accept_event);if(timer)CloseHandle(timer);if(wake)CloseHandle(wake);WSACleanup();}
    void post(Message m){
        std::lock_guard lock(mutex);
        if(m.kind==1 || m.kind==3)tx.clear();
        if(m.kind==4 && std::count_if(inbox.begin(),inbox.end(),[](const auto &v){return v.kind==4;})>=4){const auto old=std::find_if(inbox.begin(),inbox.end(),[](const auto &v){return v.kind==4;});inbox.erase(old);}
        if(inbox.size()>=4096){
            const auto old=std::find_if(inbox.begin(),inbox.end(),[](const auto &v){return v.kind==4;});
            if(old!=inbox.end())inbox.erase(old);else throw usbip::Error("Native USB mailbox overflow");
        }inbox.push_back(std::move(m));SetEvent(wake);
    }
    void publish(bool connected=true){
        if(!status_writer)return;const auto &s=core.stats();const auto now=usbip::Core::Clock::now();
        const auto current_life=std::tuple{connected,s.wcid,s.generation,peer_error,address_revision.load(),imported!=nullptr,usb_closed,last_usb_error};
        if(published_life==current_life && now-last_status<std::chrono::seconds(2))return;
        std::ostringstream json;json<<std::boolalpha<<"{\"Pid\":"<<product<<",\"Connector\":\""<<device_connector(product).name<<"\",\"UsbImported\":"<<(imported!=nullptr)<<",\"VirtualSerial\":\""<<profile.virtual_serial<<"\",\"AssociationId\":"<<association_id<<",\"RadioConnected\":"<<connected<<",\"Ready\":"<<(connected && core.ready())<<",\"Wcid\":"<<unsigned(s.wcid)<<",\"Generation\":"<<(association_id*100+s.generation)
            <<",\"Backend\":\"cpp\",\"PeerProtocolError\":"<<peer_error<<",\"Mac\":\"";
        {std::lock_guard lock(mutex);constexpr char hex[]="0123456789abcdef";for(auto b:mac)json<<hex[b>>4]<<hex[b&15];}
        json<<"\",\"PowerCommands\":"<<s.power_commands<<",\"LastPower\":\""<<s.last_power<<"\",\"UsbModeSwitchSuppressed\":"<<s.mode_suppressed<<",\"UsbClosed\":"<<usb_closed<<",\"LastUsbError\":"<<Json(last_usb_error).dump();
        if(!diagnostics){json<<'}';status_writer->publish(json.str());published_life=current_life;last_status=now;return;}
        json
            <<",\"RxPackets\":"<<s.rx<<",\"TxPackets\":"<<s.tx<<",\"AudioBridge\":"<<audio<<",\"AudioLayoutSupported\":"<<s.layout
            <<",\"AudioRxPackets\":"<<s.audio_rx<<",\"AudioTxPackets\":"<<s.audio_tx<<",\"UsbAudioInCompletions\":"<<s.capture_slots<<",\"CaptureDroppedFrames\":"<<s.dropped
            <<",\"CaptureQueueHighWater\":"<<s.capture_high_water<<",\"RenderExpiredRequests\":"<<s.render_expired<<",\"CaptureConcealedFrames\":"<<s.concealed<<",\"UsbModeSwitchSuppressed\":"<<s.mode_suppressed<<",\"Mmcss\":"<<mmcss<<",\"MmcssError\":"<<mmcss_error<<"}";
        status_writer->publish(json.str());published_life=current_life;last_status=now;
    }
    void parse(Client &c){
        for(;;){
            if(c.phase==0){
                if(c.input.size()<8)return;
                if(c.input[0]!=1 || c.input[1]!=17 || c.input[2]!=128 || std::any_of(c.input.begin()+4,c.input.begin()+8,[](uint8_t b){return b!=0;}))throw usbip::Error("Invalid USB/IP handshake");
                const auto operation=c.input[3];c.input.erase(c.input.begin(),c.input.begin()+8);
                if(operation==5){c.send(profile.device_list());c.phase=3;return;}
                if(operation!=3)throw usbip::Error("Unsupported USB/IP operation");c.phase=1;
            }else if(c.phase==1){
                if(c.input.size()<32)return;Bytes bus(32);bus[0]='1';bus[1]='-';bus[2]='1';
                const bool ok=core.ready() && !imported && std::equal(bus.begin(),bus.end(),c.input.begin());c.input.erase(c.input.begin(),c.input.begin()+32);c.send(import_header(ok));
                if(!ok){c.phase=3;return;}core.imported();imported=&c;c.phase=2;c.send(profile.metadata());
            }else if(c.phase==2){
                const auto parsed=usbip::request(c.input);if(!parsed)return;auto r=std::move(parsed->first);c.input.erase(c.input.begin(),c.input.begin()+parsed->second);core.submit(std::move(r));
            }else return;
        }
    }
    void service(){
        streaming_audio=audio && core.ready() && core.stats().layout;
        if(imported && !imported->dead){for(auto &reply:core.replies())imported->send(std::move(reply));}else core.replies();
        auto requests=core.outbound();
        {std::lock_guard lock(mutex);
            const bool reset_pending=std::any_of(inbox.begin(),inbox.end(),[](const auto &m){return m.kind==1 || m.kind==3;});
            if(!reset_pending){if(tx.size()+requests.size()>128)throw usbip::Error("Native radio TX queue overflow");for(auto &request:requests)tx.push_back(std::move(request));}
        }
        const auto now=usbip::Core::Clock::now();
        // A live USB interrupt URB may wait indefinitely on an idle device.
        // Limit only incomplete imports; TCP close and RF generation own lifetime.
        for(auto &c:clients){if(c->phase!=2 && now-c->last>std::chrono::seconds(5))c->dead=true;if(!c->dead)c->flush();}
        std::erase_if(clients,[&](const auto &c){if(!c->dead)return false;if(c.get()==imported){++usb_closed;core.detached();imported=nullptr;std::lock_guard lock(mutex);tx.clear();}return true;});
        publish();
    }
    void run(){
        DWORD task_index=0;HANDLE scheduling=audio?AvSetMmThreadCharacteristicsW(L"Pro Audio",&task_index):nullptr;
        mmcss=scheduling!=nullptr;if(audio && !mmcss)mmcss_error=GetLastError();
        try{
            publish();
            for(;;){
                std::deque<Message> events;
                {std::lock_guard lock(mutex);if(stopping)break;events.swap(inbox);ResetEvent(wake);}
                for(const auto &m:events){
                    if(m.kind==4 && usbip::Core::Clock::now()-m.arrived>std::chrono::milliseconds(32))continue;
                    if(m.kind==1 || m.kind==3){std::lock_guard lock(mutex);tx.clear();}
                    if(m.kind==1 || m.kind==3)for(auto &client:clients)client->dead=true;
                    try{
                        if(m.kind==17)core.complete(m.wcid,m.token,m.status);else core.radio(m.kind,m.wcid,m.data);
                        if(m.kind==1)peer_error=false;
                    }catch(const usbip::Error &){
                        peer_error=true;core.radio(3,core.stats().wcid,{});
                        std::lock_guard lock(mutex);tx.clear();
                    }
                }
                core.tick();service();
                auto deadline=usbip::Core::Clock::now()+std::chrono::seconds(2);if(const auto needed=core.wake_at())deadline=std::min(deadline,*needed);
                const auto delay=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-usbip::Core::Clock::now()).count();LARGE_INTEGER due;due.QuadPart=-std::max(int64_t(1),static_cast<int64_t>(delay/100));
                if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))throw usbip::Error("Cannot schedule native USB timer");
                std::vector<HANDLE> handles{wake,timer,accept_event};for(const auto &c:clients)handles.push_back(c->event);
                const auto ready=WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,INFINITE);
                if(ready==WAIT_FAILED || ready>=WAIT_OBJECT_0+handles.size())throw usbip::Error("Native USB wait failed");
                const auto index=ready-WAIT_OBJECT_0;
                if(index==2){
                    WSANETWORKEVENTS flags{};if(WSAEnumNetworkEvents(listener,accept_event,&flags))throw usbip::Error("Native USB listener failed");
                    for(;;){auto s=accept(listener,nullptr,nullptr);if(s==INVALID_SOCKET){if(WSAGetLastError()!=WSAEWOULDBLOCK)throw usbip::Error("USB accept failed");break;}if(clients.size()>=16){closesocket(s);continue;}clients.push_back(std::make_unique<Client>(s));}
                }else if(index>=3){
                    auto &c=*clients[index-3];WSANETWORKEVENTS flags{};
                    try{if(WSAEnumNetworkEvents(c.socket,c.event,&flags))throw usbip::Error("USB socket failed");
                        if(flags.lNetworkEvents&FD_READ){c.read();if(!c.dead)parse(c);}if(flags.lNetworkEvents&FD_CLOSE)c.dead=true;if(!c.dead)c.flush();
                    }catch(const usbip::Error &e){last_usb_error=e.what();c.dead=true;}
                }
            }
        }catch(const std::exception &e){std::lock_guard lock(mutex);error=e.what();}
        publish(false);if(scheduling)AvRevertMmThreadCharacteristics(scheduling);
    }
};
NativeUsbOutput::NativeUsbOutput(uint16_t port,Bytes mac,uint16_t pid,bool audio,std::wstring status,bool automatic,bool diagnostics):impl_(std::make_unique<Impl>(port,std::move(mac),pid,audio,status,automatic,diagnostics)){}
NativeUsbOutput::~NativeUsbOutput()=default;
bool NativeUsbOutput::target(std::span<const uint8_t> mac)const{std::lock_guard lock(impl_->mutex);return std::equal(mac.begin(),mac.end(),impl_->mac.begin(),impl_->mac.end());}
bool NativeUsbOutput::automatic()const{return impl_->automatic;}
bool NativeUsbOutput::audio_active()const{return impl_->streaming_audio.load();}
bool NativeUsbOutput::claim(std::span<const uint8_t> mac,uint16_t vendor,uint16_t product){
    if(!impl_->automatic || vendor!=0x045e || usb_model(product)!=impl_->product || mac.size()!=6)return false;
    std::lock_guard lock(impl_->mutex);if(impl_->bound_wcid)return false;impl_->mac.assign(mac.begin(),mac.end());++impl_->address_revision;return true;
}
void NativeUsbOutput::event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data){
    {std::lock_guard lock(impl_->mutex);if(kind==1)impl_->bound_wcid=wcid;else if(kind==3)impl_->bound_wcid=0;}
    impl_->post({kind,wcid,0,0,{data.begin(),data.end()}});
}
std::vector<RawGipTransmit> NativeUsbOutput::poll(){std::lock_guard lock(impl_->mutex);if(!impl_->error.empty())throw usbip::Error(impl_->error);auto out=std::move(impl_->tx);impl_->tx.clear();return out;}
void NativeUsbOutput::complete(uint8_t wcid,uint32_t token,int status){impl_->post({17,wcid,token,status,{}});}
}

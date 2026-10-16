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
#include "diagnostic_log.hpp"
#include "control_media_queue.hpp"
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
    bool stopping=false;std::string error;ControlMediaQueue<Message> inbox{4096,4,4};std::vector<RawGipTransmit> tx;
    uint64_t reported_mailbox_drops=0;usbip::Core::Clock::time_point mailbox_reported_at{};
    std::string last_usb_error;uint64_t usb_closed=0;std::atomic_uint64_t mailbox_dropped=0;
    std::vector<std::unique_ptr<Client>> clients;Client *imported=nullptr;
    std::unique_ptr<LatestSnapshot> status_writer;NativeUsbOutput::StateSink state_sink;
    usbip::Core::Clock::time_point last_status{};bool mmcss=false,mmcss_requested=false,peer_error=false;DWORD mmcss_error=0;
    uint64_t worker_iterations=0,max_mailbox_delay_us=0,max_radio_batch_us=0,max_socket_dispatch_us=0;
    std::atomic_uint64_t address_revision=0;
    std::atomic_bool streaming_audio=false;
    uint64_t association_id=0;
    std::optional<std::tuple<bool,uint8_t,uint64_t,bool,uint64_t,bool,uint64_t,std::string,bool>> published_life;
    Impl(uint16_t port,Bytes address,uint16_t pid,bool with_audio,const std::wstring &status,bool auto_select,bool trace,NativeUsbOutput::StateSink sink):mac(std::move(address)),profile(usbip::profile(pid,with_audio)),core(profile,with_audio),audio(with_audio),automatic(auto_select),diagnostics(trace),product(pid){
        state_sink=std::move(sink);
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
                    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!out || !out.write(text.data(),static_cast<std::streamsize>(text.size()))){diagnostic("status_write_failed",{},true);return;}}
                    for(unsigned attempt=0;attempt<20;++attempt){if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
                    diagnostic("status_replace_failed",{},true);
                });
            }
            thread=std::thread([this]{run();});
        }catch(...){release();throw;}
    }
    ~Impl(){ {std::lock_guard lock(mutex);stopping=true;}SetEvent(wake);if(thread.joinable())thread.join();clients.clear();release(); }
    void release(){if(listener!=INVALID_SOCKET)closesocket(listener);if(accept_event!=WSA_INVALID_EVENT)WSACloseEvent(accept_event);if(timer)CloseHandle(timer);if(wake)CloseHandle(wake);WSACleanup();}
    void report_mailbox_drops(){
        const auto total=inbox.dropped_media();mailbox_dropped=total;const auto now=usbip::Core::Clock::now();
        if(total!=reported_mailbox_drops && now-mailbox_reported_at>=std::chrono::seconds(1)){
            diagnostic("mailbox_audio_dropped",{{"association",association_id},{"count",total-reported_mailbox_drops},{"total",total},{"control_high_water",inbox.reliable_high_water()}},true);
            reported_mailbox_drops=total;mailbox_reported_at=now;
        }
    }
    void post(Message m){
        std::lock_guard lock(mutex);
        if(m.kind==1 || m.kind==3){tx.clear();inbox.discard_media([](const auto &){return true;});}
        const auto media=m.kind==4;const auto wcid=m.wcid;const auto arrived=m.arrived;
        try{inbox.push(std::move(m),media,wcid,arrived);}
        catch(const std::overflow_error &){diagnostic("control_mailbox_overflow",{{"association",association_id},{"queued",inbox.reliable_size()}},true);throw;}
        report_mailbox_drops();SetEvent(wake);
    }
    void publish(bool connected=true){
        if(!status_writer && !state_sink)return;const auto &s=core.stats();const auto now=usbip::Core::Clock::now();
        const auto current_life=std::tuple{connected,s.wcid,s.generation,peer_error,address_revision.load(),imported!=nullptr,usb_closed,last_usb_error,s.recovery_required};
        if(published_life==current_life && now-last_status<std::chrono::seconds(2))return;
        FILETIME created{},exited{},kernel{},user{};uint64_t cpu_100ns=0;
        if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))cpu_100ns=(uint64_t(kernel.dwHighDateTime)<<32|kernel.dwLowDateTime)+(uint64_t(user.dwHighDateTime)<<32|user.dwLowDateTime);
        const auto &init=core.initialization();
        std::ostringstream json;json<<std::boolalpha<<"{\"Pid\":"<<product<<",\"Connector\":\""<<device_connector(product).name<<"\",\"UsbImported\":"<<(imported!=nullptr)<<",\"VirtualSerial\":\""<<profile.virtual_serial<<"\",\"AssociationId\":"<<association_id<<",\"RadioConnected\":"<<connected<<",\"Ready\":"<<(connected && core.ready())<<",\"Wcid\":"<<unsigned(s.wcid)<<",\"Generation\":"<<(association_id*100+s.generation)
            <<",\"Backend\":\"cpp\",\"PeerProtocolError\":"<<peer_error<<",\"Mac\":\"";
        {std::lock_guard lock(mutex);constexpr char hex[]="0123456789abcdef";for(auto b:mac)json<<hex[b>>4]<<hex[b&15];}
        json<<"\",\"PowerCommands\":"<<s.power_commands<<",\"LastPower\":\""<<s.last_power<<"\",\"UsbModeSwitchSuppressed\":"<<s.mode_suppressed<<",\"UsbClosed\":"<<usb_closed<<",\"LastUsbError\":"<<Json(last_usb_error).dump();
        json<<",\"RecoveryRequired\":"<<s.recovery_required<<",\"RadioAssociated\":"<<(connected && core.associated())<<",\"MailboxDroppedAudio\":"<<mailbox_dropped.load()
            <<",\"UsbImports\":"<<s.usb_imports<<",\"AnnounceReplays\":"<<s.announce_replays<<",\"CaptureWithoutUsb\":"<<s.capture_without_usb
            <<",\"Mmcss\":"<<mmcss<<",\"MmcssRequested\":"<<mmcss_requested<<",\"MmcssError\":"<<mmcss_error
            <<",\"WorkerCpu100ns\":"<<cpu_100ns<<",\"WorkerIterations\":"<<worker_iterations<<",\"MaxMailboxDelayUs\":"<<max_mailbox_delay_us<<",\"MaxRadioBatchUs\":"<<max_radio_batch_us<<",\"MaxSocketDispatchUs\":"<<max_socket_dispatch_us
            <<",\"UsbInitialization\":{\"Configurations\":"<<init.configurations<<",\"Interfaces\":"<<init.interfaces<<",\"InterruptIn\":"<<init.irq_in<<",\"InterruptOut\":"<<init.irq_out<<",\"InterruptDelivered\":"<<init.irq_delivered<<",\"Capture\":"<<init.capture<<",\"Render\":"<<init.render<<",\"GipCommands\":"<<init.commands<<'}';
        if(!diagnostics && !state_sink){json<<'}';if(state_sink && !state_sink(json.str(),published_life!=current_life))return;if(status_writer)status_writer->publish(json.str());published_life=current_life;last_status=now;return;}
        json
            <<",\"RxPackets\":"<<s.rx<<",\"TxPackets\":"<<s.tx<<",\"AudioBridge\":"<<audio<<",\"AudioLayoutSupported\":"<<s.layout
            <<",\"AudioRxPackets\":"<<s.audio_rx<<",\"AudioTxPackets\":"<<s.audio_tx<<",\"UsbAudioInCompletions\":"<<s.capture_slots<<",\"CaptureDroppedFrames\":"<<s.dropped
            <<",\"CaptureQueueHighWater\":"<<s.capture_high_water<<",\"RenderExpiredRequests\":"<<s.render_expired<<",\"CaptureConcealedFrames\":"<<s.concealed<<",\"UsbModeSwitchSuppressed\":"<<s.mode_suppressed<<"}";
        if(state_sink && !state_sink(json.str(),published_life!=current_life))return;
        if(status_writer)status_writer->publish(json.str());published_life=current_life;last_status=now;
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
                const bool ok=core.associated() && !imported && std::equal(bus.begin(),bus.end(),c.input.begin());c.input.erase(c.input.begin(),c.input.begin()+32);c.send(import_header(ok));
                if(!ok){c.phase=3;return;}core.imported();imported=&c;c.phase=2;c.send(profile.metadata());
                diagnostic("usb_imported",{{"association",association_id},{"wcid",core.stats().wcid},{"generation",core.stats().generation}});
            }else if(c.phase==2){
                const auto parsed=usbip::request(c.input);if(!parsed)return;auto r=std::move(parsed->first);c.input.erase(c.input.begin(),c.input.begin()+parsed->second);core.submit(std::move(r));
            }else return;
        }
    }
    void service(){
        streaming_audio=audio && core.ready() && core.stats().layout;
        if(imported && !imported->dead){for(auto &reply:core.replies())imported->send(std::move(reply));}else core.replies();
        auto requests=core.outbound();
        std::vector<RawGipTransmit> expired;
        {std::lock_guard lock(mutex);
            const bool reset_pending=core.stats().recovery_required || inbox.any_reliable([](const auto &m){return m.kind==1 || m.kind==3;});
            if(reset_pending)tx.clear();
            if(!reset_pending){
                size_t render=static_cast<size_t>(std::count_if(tx.begin(),tx.end(),[](const auto &r){return r.audio;})),control=tx.size()-render;
                for(auto &request:requests){
                    if(request.audio){
                        if(render>=4 || usbip::Core::Clock::now()>request.media_deadline){expired.push_back(std::move(request));continue;}
                        ++render;
                    }else if(control++>=128){diagnostic("control_tx_queue_overflow",{{"association",association_id}},true);throw usbip::Error("Native radio control TX queue overflow");}
                    tx.push_back(std::move(request));
                }
            }
        }
        for(const auto &request:expired)core.complete(request.wcid,request.token,-18);
        const auto now=usbip::Core::Clock::now();
        // A live USB interrupt URB may wait indefinitely on an idle device.
        // Limit only incomplete imports; TCP close and RF generation own lifetime.
        for(auto &c:clients){if(c->phase!=2 && now-c->last>std::chrono::seconds(5))c->dead=true;if(!c->dead)c->flush();}
        std::erase_if(clients,[&](const auto &c){if(!c->dead)return false;if(c.get()==imported){++usb_closed;diagnostic("usb_import_closed",{{"association",association_id},{"wcid",core.stats().wcid},{"count",usb_closed}},core.associated());core.detached();imported=nullptr;std::lock_guard lock(mutex);tx.clear();}return true;});
        publish();
    }
    void run(){
        wchar_t setting[2]{};const auto setting_length=GetEnvironmentVariableW(L"XONE_USB_MMCSS",setting,2);
        mmcss_requested=audio && !(setting_length==1 && setting[0]==L'0');
        DWORD task_index=0;HANDLE scheduling=mmcss_requested?AvSetMmThreadCharacteristicsW(L"Pro Audio",&task_index):nullptr;
        mmcss=scheduling!=nullptr;if(mmcss_requested && !mmcss)mmcss_error=GetLastError();
        diagnostic("usb_worker_scheduling",{{"association",association_id},{"mmcss_requested",mmcss_requested},{"mmcss",mmcss},{"error",mmcss_error},{"thread_priority",GetThreadPriority(GetCurrentThread())}});
        try{
            publish();
            for(;;){
                ++worker_iterations;const auto batch_started=usbip::Core::Clock::now();
                std::deque<Message> events;
                {std::lock_guard lock(mutex);if(stopping)break;events=inbox.drain();report_mailbox_drops();if(inbox.empty())ResetEvent(wake);else SetEvent(wake);}
                for(const auto &m:events){
                    max_mailbox_delay_us=std::max(max_mailbox_delay_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(usbip::Core::Clock::now()-m.arrived).count()));
                    if(m.kind==1 || m.kind==3){std::lock_guard lock(mutex);tx.clear();}
                    if(m.kind==1 || m.kind==3)for(auto &client:clients)client->dead=true;
                    try{
                        if(m.kind==17)core.complete(m.wcid,m.token,m.status);else core.radio(m.kind,m.wcid,m.data);
                        if(m.kind==1)peer_error=false;
                    }catch(const usbip::Error &e){
                        diagnostic("peer_protocol_error",{{"association",association_id},{"wcid",m.wcid},{"error",e.what()}},true);
                        peer_error=true;core.radio(3,core.stats().wcid,{});
                        {std::lock_guard lock(mutex);tx.clear();}
                        // Report through poll/router to the RF session. Keeping
                        // a reset core behind a live association cannot recover.
                        throw;
                    }
                }
                core.tick();service();
                max_radio_batch_us=std::max(max_radio_batch_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(usbip::Core::Clock::now()-batch_started).count()));
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
                    const auto dispatch_started=usbip::Core::Clock::now();
                    auto &c=*clients[index-3];WSANETWORKEVENTS flags{};
                    try{if(WSAEnumNetworkEvents(c.socket,c.event,&flags))throw usbip::Error("USB socket failed");
                        if(flags.lNetworkEvents&FD_READ){c.read();if(!c.dead)parse(c);}if(flags.lNetworkEvents&FD_CLOSE)c.dead=true;if(!c.dead)c.flush();
                    }catch(const usbip::Error &e){last_usb_error=e.what();diagnostic("usb_client_error",{{"association",association_id},{"error",last_usb_error}},true);c.dead=true;}
                    max_socket_dispatch_us=std::max(max_socket_dispatch_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(usbip::Core::Clock::now()-dispatch_started).count()));
                }
            }
        }catch(const std::exception &e){std::lock_guard lock(mutex);error=e.what();diagnostic("usb_worker_failed",{{"association",association_id},{"error",error}},true);}
        publish(false);if(scheduling)AvRevertMmThreadCharacteristics(scheduling);
    }
};
NativeUsbOutput::NativeUsbOutput(uint16_t port,Bytes mac,uint16_t pid,bool audio,std::wstring status,bool automatic,bool diagnostics,StateSink sink):impl_(std::make_unique<Impl>(port,std::move(mac),pid,audio,status,automatic,diagnostics,std::move(sink))){}
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

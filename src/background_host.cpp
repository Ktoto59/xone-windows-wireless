// SPDX-License-Identifier: GPL-2.0-or-later
// Invisible user-session host with bounded asynchronous diagnostic journals.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <usbiodef.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <optional>
#include "appliance_audio.hpp"
#include "winusb_transport.hpp"
#include "adapter_catalog.hpp"
#include "usb_import_recovery.hpp"
#include "radio_restart_backoff.hpp"
#include "host_cleanup.hpp"
#include "owned_usb_port.hpp"
#include "usb_attach_recovery.hpp"
#include "peer_recovery_backoff.hpp"
#include "diagnostic_log.hpp"
#include "runtime_state.hpp"
#include <iostream>
namespace {
using xone::Json;namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
struct Handle {HANDLE value=nullptr;~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle()=default;explicit Handle(HANDLE h):value(h){};Handle(const Handle &)=delete;Handle &operator=(const Handle &)=delete;};
fs::path root,binary_dir;
bool hardware_changed=true,resumed=false;
Json read(const fs::path &path){
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return Json::object();const auto size=in.tellg();if(size<0 || size>65536)throw std::runtime_error("Local state exceeds size limit");
    std::string text(static_cast<size_t>(size),0);in.seekg(0);if(!in.read(text.data(),size))return Json::object();auto result=Json::parse(text,nullptr,false);return result.is_discarded()?Json::object():result;
}
void write(const fs::path &path,const Json &json){
    const auto temporary=fs::path(path.wstring()+L".tmp");fs::create_directories(path.parent_path());const auto text=json.dump();
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!out || !out.write(text.data(),static_cast<std::streamsize>(text.size())))throw std::runtime_error("Cannot save application state");}
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("Cannot replace application state");
}
std::wstring quote(std::wstring_view text){
    std::wstring out=L"\"";unsigned slashes=0;
    for(auto c:text){if(c==L'\\'){++slashes;continue;}out.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;if(c==L'"')out+=L'\\';out+=c;}out.append(slashes*2,L'\\');return out+L'"';
}
uint64_t ticks(HANDLE process){FILETIME created{},exit{},kernel{},user{};if(!GetProcessTimes(process,&created,&exit,&kernel,&user))throw std::runtime_error("Cannot identify child process");return (uint64_t(created.dwHighDateTime)<<32|created.dwLowDateTime)+504911232000000000ULL;}
Json identity(HANDLE process,DWORD pid){return {{"Pid",pid},{"StartTicks",ticks(process)}};}
struct Child {
    Handle process,job;DWORD pid=0;
    void start(const fs::path &exe,std::wstring arguments,HANDLE output=nullptr){
        if(process.value || job.value)throw std::runtime_error("Child is already owned");
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle nul(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=nul.value;startup.hStdOutput=output?output:nul.value;startup.hStdError=nul.value;
        auto command=quote(exe.wstring())+L" "+arguments;PROCESS_INFORMATION info{};
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,root.c_str(),&startup,&info))throw std::runtime_error("Cannot start application dependency");
        process.value=info.hProcess;pid=info.dwProcessId;Handle thread(info.hThread);job.value=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job.value || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) || !AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);throw std::runtime_error("Cannot own application child");}
        if(ResumeThread(thread.value)==DWORD(-1)){close();throw std::runtime_error("Cannot resume application child");}
    }
    bool alive()const{return process.value && WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;}
    bool close()noexcept{
        // Close the entire job first, including descendants of an exited CLI.
        // Retain the process handle until its termination has been observed.
        if(job.value){CloseHandle(job.value);job.value=nullptr;}
        bool stopped=true;
        if(process.value){
            if(WaitForSingleObject(process.value,0)!=WAIT_OBJECT_0){TerminateProcess(process.value,1);stopped=WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0;}
            CloseHandle(process.value);process.value=nullptr;
        }
        pid=0;return stopped;
    }
    ~Child(){close();}
};
fs::path usbip_path(){wchar_t folder[32768]{};const auto length=GetEnvironmentVariableW(L"ProgramW6432",folder,32768);if(!length || length>=32768)throw std::runtime_error("Cannot locate USBip installation");return fs::path(folder)/L"USBip/usbip.exe";}
int run_usbip(const fs::path &exe,std::wstring arguments,bool get_port,std::string *captured,Clock::time_point deadline,HANDLE cancellation=nullptr){
    if(Clock::now()>=deadline)throw std::runtime_error("USBip cleanup deadline exhausted");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE r=nullptr,w=nullptr;if(!CreatePipe(&r,&w,&security,4096))throw std::runtime_error("Cannot create dependency response pipe");Handle reader(r),writer(w);SetHandleInformation(r,HANDLE_FLAG_INHERIT,0);
    Child child;child.start(exe,std::move(arguments),w);CloseHandle(writer.value);writer.value=nullptr;
    std::string output;
    while(child.alive() && Clock::now()<deadline){if(cancellation && WaitForSingleObject(cancellation,0)==WAIT_OBJECT_0)throw std::runtime_error("USBip operation cancelled");DWORD available=0;if(PeekNamedPipe(r,nullptr,0,nullptr,&available,nullptr) && available){char bytes[512];DWORD got=0;if(!ReadFile(r,bytes,std::min<DWORD>(available,sizeof(bytes)),&got,nullptr))break;output.append(bytes,got);if(output.size()>4096)throw std::runtime_error("Invalid dependency response");}WaitForSingleObject(child.process.value,50);}
    if(child.alive())throw std::runtime_error("USBip operation timed out");
    char bytes[512];DWORD available=0,got=0;while(PeekNamedPipe(r,nullptr,0,nullptr,&available,nullptr) && available && ReadFile(r,bytes,std::min<DWORD>(available,sizeof(bytes)),&got,nullptr)){output.append(bytes,got);if(output.size()>4096)throw std::runtime_error("Invalid dependency response");}
    if(captured)*captured=output;DWORD code=1;GetExitCodeProcess(child.process.value,&code);if(code)throw std::runtime_error("USBip operation failed (exit "+std::to_string(code)+")");if(!get_port)return 0;
    const auto first=output.find_first_not_of(" \t\r\n"),last=output.find_last_not_of(" \t\r\n");if(first==output.npos)throw std::runtime_error("Missing imported port");const auto number=output.substr(first,last-first+1);
    if(number.size()>4 || !std::all_of(number.begin(),number.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("Invalid imported port");const int port=std::stoi(number);if(port<1 || port>65535)throw std::runtime_error("Invalid imported port");return port;
}
int usbip(std::wstring arguments,bool get_port,std::string *captured=nullptr,Clock::time_point deadline=Clock::now()+std::chrono::seconds(15),HANDLE cancellation=nullptr){
    return run_usbip(usbip_path(),std::move(arguments),get_port,captured,deadline,cancellation);
}
LRESULT CALLBACK events(HWND window,UINT message,WPARAM w,LPARAM l){
    if(message==WM_DEVICECHANGE)hardware_changed=true;
    if(message==WM_POWERBROADCAST && (w==PBT_APMRESUMEAUTOMATIC || w==PBT_APMRESUMESUSPEND)){resumed=true;hardware_changed=true;}
    return DefWindowProcW(window,message,w,l);
}
struct Window {
    HWND handle=nullptr;HDEVNOTIFY notification=nullptr;
    Window(){WNDCLASSW cls{};cls.lpfnWndProc=events;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"XoneBackgroundEvents";RegisterClassW(&cls);handle=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",0,0,0,0,0,nullptr,nullptr,cls.hInstance,nullptr);if(!handle)throw std::runtime_error("Cannot receive device notifications");DEV_BROADCAST_DEVICEINTERFACE_W filter{};filter.dbcc_size=sizeof(filter);filter.dbcc_devicetype=DBT_DEVTYP_DEVICEINTERFACE;filter.dbcc_classguid=GUID_DEVINTERFACE_USB_DEVICE;notification=RegisterDeviceNotificationW(handle,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);}
    ~Window(){if(notification)UnregisterDeviceNotification(notification);if(handle)DestroyWindow(handle);}
    void pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
};
bool healthy(const std::wstring &id){DEVINST node=0;ULONG status=0,problem=0;return CM_Locate_DevNodeW(&node,const_cast<PWSTR>(id.c_str()),CM_LOCATE_DEVNODE_NORMAL)==CR_SUCCESS && CM_Get_DevNode_Status(&status,&problem,node,0)==CR_SUCCESS && (status&DN_STARTED) && !(status&DN_HAS_PROBLEM);}
struct Peer {xone::ApplianceDevice model;int port=0;uint64_t generation=0;bool ready=false;Clock::time_point attached_at{};xone::PeerRecoveryBackoff retry;std::string error;xone::UsbImportRecovery import;xone::UsbAttachRecovery attach;};
void peer_failed(Peer &p,const std::string &error,Clock::time_point now){
    p.retry.failed(now);p.ready=false;p.error=error;
    xone::diagnostic("slot_retry",{{"slot",p.model.name},{"error",error},{"attempt",p.retry.failures()},{"retry_seconds",p.retry.delay().count()}},true);
}
// Shared by the host and fault-injection tests; callbacks replace only the OS
// boundary. Normal lifecycle changes never enter the exception/failure path.
template<class Attach,class Detach,class Save>
void service_peer_snapshot(Peer &p,const Json &s,Clock::time_point now,Attach attach,Detach detach,Save save){
    if(!s.is_object() || !s.contains("Generation") || !s["Generation"].is_number_unsigned() || !s.contains("Ready") || !s["Ready"].is_boolean()){
        p.retry.interrupted();return;
    }
    const auto generation=s.value("Generation",uint64_t(0));const bool ready=s.value("Ready",false);
    const bool recovery=s.value("RecoveryRequired",false),associated=s.value("RadioAssociated",ready);
    const bool known_import=s.contains("UsbImported") && s["UsbImported"].is_boolean();
    if(p.port){
        const auto reason=p.import.detach_reason(ready,recovery,generation,p.generation,s.value("UsbImported",false),known_import,s.value("SnapshotSequence",uint64_t(0)),s.value("UsbImports",uint64_t(0)),s.value("UsbClosed",uint64_t(0)),now,s.value("AssociationId",uint64_t(0)),s.value("PublishedTickMs",uint64_t(0)));
        if(reason!=xone::UsbDetachReason::none){
            xone::diagnostic("usb_reimport_required",{{"slot",p.model.name},{"port",p.port},{"reason",xone::usb_detach_name(reason)},{"generation",generation},{"previous_generation",p.generation}},xone::usb_detach_failed(reason));
            // Invalidate before cleanup, so a throwing detach is retried even
            // if the RF snapshot later disappears or becomes healthy again.
            p.generation=0;p.ready=false;p.retry.interrupted();detach(p);
            // Schedule actual failures at completion of the synchronous cleanup,
            // so time spent in USBip cannot consume the retry delay.
            if(xone::usb_detach_failed(reason))throw xone::UsbImportFailure(reason);
            return; // observe the next RF snapshot before a new attach
        }
    }
    p.ready=ready;
    if(xone::UsbImportRecovery::attach(ready,recovery,associated)){
        const auto pid=s.value("Pid",uint16_t(0));if(pid!=0x0b17 && pid!=0x0b32 && pid!=0x0b12)throw std::runtime_error("Unknown native slot model");
        p.model.pid=pid;p.model.audio=pid!=0x0b12;p.model.profile=pid==0x0b17?"headset-0b17-usb.json":pid==0x0b32?"headset-0b32-usb.json":"controller-0b12-usb.json";
    }
    if(xone::UsbImportRecovery::attach(ready,recovery,associated) && !p.port && p.retry.ready(now)){
        p.retry.interrupted();attach(p);p.generation=generation;
        const auto mac=s.value("Mac",p.model.mac);if(mac.size()==12 && std::all_of(mac.begin(),mac.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))p.model.mac=mac;
        save();return; // the pre-attach snapshot cannot prove a healthy import
    }
    if(p.retry.observe_healthy(p.port && p.import.seen_import && ready && !recovery && known_import && s.value("UsbImported",false),now))p.error.clear();
}
class Host {
    std::array<Peer,8> peers_;Json config_,previous_state_;Child radio_;std::wstring adapter_;Window window_;
    xone::RuntimeState memory_{root,true};Json registry_;bool registry_dirty_=false;
    Clock::time_point adapter_check_{};xone::RadioRestartBackoff radio_retry_;std::string retry_reason_;
    DWORD last_radio_exit_=0;Json cleanup_errors_=Json::array();
    fs::path work=root/L"work";Json me_=identity(GetCurrentProcess(),GetCurrentProcessId());
    fs::path peer_status(const Peer &peer)const{return work/xone::wide("native-"+peer.model.name+".json");}
    void state(std::string phase,std::string error={}){
        Json ready=Json::array(),devices=Json::array();for(const auto &p:peers_){if(p.ready && p.port)ready.push_back(p.model.name);devices.push_back({{"Name",p.model.name},{"Mac",p.model.mac},{"Audio",p.model.audio},{"Port",p.port},{"Generation",p.generation},{"Status",xone::narrow(peer_status(p).wstring())},{"UsbPort",p.model.port},{"Error",p.error},{"RetryCount",p.retry.failures()}});}
        Json status={{"Phase",phase},{"Error",error.empty()?Json(nullptr):Json(error)},{"Process",me_},{"WorkerActive",radio_.alive()},{"RadioActive",radio_.alive()},{"DevicesReady",ready},{"Devices",devices},{"AudioSelected",false},{"RetryCount",radio_retry_.failures()},{"RetryDelaySeconds",radio_retry_.delay().count()},{"LastRadioExit",last_radio_exit_},{"Backend","cpp-headless"},{"CleanupErrors",cleanup_errors_}};
        if(status==previous_state_)return;
        xone::diagnostic("host_state",{{"phase",phase},{"error",error},{"ready",ready},{"last_radio_exit",last_radio_exit_}},!error.empty());
        if(!memory_.publish(xone::RuntimeState::host,status.dump()))return;
        const Json running={{"Running",radio_.alive()},{"FallbackOutput","none"},{"NativeController",true},{"NewHeadsetOnly",false},{"BridgeBackend","cpp"},{"Worker",me_},{"Radio",radio_.process.value?identity(radio_.process.value,radio_.pid):Json(nullptr)},{"Devices",devices},{"Error",status["Error"]}};
        if(!memory_.publish(xone::RuntimeState::running,running.dump()))return;
        previous_state_=status;
    }
    void detach(Peer &peer,Clock::time_point deadline=Clock::now()+std::chrono::seconds(15),bool stopping=false){
        peer.ready=false;peer.generation=0;peer.retry.interrupted();
        peer.attach.reconcile(peer.port,peer.model.port,[&](const std::wstring &args,bool get_port,std::string *captured){
            return usbip(args,get_port,captured,deadline,stopping?nullptr:memory_.stop_event());
        });
        // The CLI serial is empty: attach deliberately has no --serial override.
        // It differs from the server VirtualSerial (which Windows still reads).
        xone::detach_owned_usb_port(peer.port,peer.model.port,"",[&](const std::wstring &args,std::string *captured){
            usbip(args,false,captured,deadline,stopping?nullptr:memory_.stop_event());
        },[&](const std::string &error){
            if(cleanup_errors_.size()<64)cleanup_errors_.push_back({{"Resource",peer.model.name},{"Error",error}});
            xone::diagnostic("usb_detach_failed",{{"slot",peer.model.name},{"port",peer.port},{"error",error}},true);
        });
        peer.port=0;peer.import={};peer.retry.interrupted();
    }
    void stop_radio(){
        radio_retry_.stopped();cleanup_errors_=Json::array();
        xone::diagnostic("radio_stop_requested");
        auto cleanup_deadline=Clock::time_point::max();
        xone::stop_owned_radio(peers_,[&]{
            if(radio_.alive()){memory_.request_radio_stop();WaitForSingleObject(radio_.process.value,8000);}
        },[&]{cleanup_deadline=Clock::now()+std::chrono::seconds(16);if(!radio_.close())throw std::runtime_error("Radio termination was not confirmed");},[&](Peer &p){
            // All eight hung USBip calls still fit the --stop 35 second bound.
            detach(p,std::min(cleanup_deadline,Clock::now()+std::chrono::seconds(2)),true);
        },[&](const std::string &resource,const std::string &error){
            cleanup_errors_.push_back({{"Resource",resource},{"Error",error}});
            xone::diagnostic("radio_cleanup_failed",{{"resource",resource},{"error",error}},true);
        });
    }
    void save_devices(){
        auto &registry=registry_;
        bool changed=false;
        for(const auto &p:peers_)if(p.ready && p.model.pid && p.model.mac!="000000000000"){
            const auto existing=std::find_if(registry["Devices"].begin(),registry["Devices"].end(),[&](const auto &d){return d.value("Mac",std::string{})==p.model.mac;});
            if(existing==registry["Devices"].end() && registry["Devices"].size()<256){const auto status=memory_.read(static_cast<size_t>(&p-peers_.data()));registry["Devices"].push_back({{"Mac",p.model.mac},{"Pid",p.model.pid},{"RadioPid",p.model.pid},{"Profile",p.model.profile},{"VirtualSerial",status.value("VirtualSerial",std::string{})}});changed=true;}
        }
        registry_dirty_|=changed;
    }
    void start_radio(){
        xone::diagnostic("radio_start_requested");
        memory_.clear_radio();for(auto &p:peers_){p.ready=false;p.generation=0;p.retry.interrupted();}
        std::wstring arguments=L"run --output usb-gip --native-usb --usb-auto-slots 8 --usb-gip-port 33400 --seconds 0 --instance "+quote(adapter_)+L" --firmware "+quote((work/L"firmware/FW_ACC_00U.bin").wstring())+L" --runtime-root "+quote(root.wstring());
        if(!config_.value("IdlePowerEnabled",true))arguments+=L" --no-idle-power";
        radio_.start(binary_dir/L"xone-probe-cpp.exe",std::move(arguments));SetPriorityClass(radio_.process.value,ABOVE_NORMAL_PRIORITY_CLASS);radio_retry_.started(Clock::now());retry_reason_.clear();state("StartingRadio");
    }
    void failed_radio(std::string reason){
        retry_reason_=std::move(reason);radio_retry_.failed(Clock::now());
        xone::diagnostic("radio_retry_scheduled",{{"reason",retry_reason_},{"failures",radio_retry_.failures()},{"delay_seconds",radio_retry_.delay().count()}},true);
    }
public:
    explicit Host(const std::wstring &selected={}){config_=read(root/L"config/devices.local.json");registry_=read(root/L"config/paired-devices.local.json");if(!registry_.contains("Devices"))registry_={{"Version",1},{"Devices",Json::array()}};if(!selected.empty())config_["AdapterInstanceId"]=xone::utf8(selected);for(size_t n=0;n<peers_.size();++n)peers_[n].model={"slot-"+std::to_string(n),"","000000000000",0,static_cast<uint16_t>(33400+2*n),false};}
    void run(){

        // Change notifications own the idle wait. No console or UI is shown.
        while(!memory_.stopped()){
            window_.pump();try{
                if(resumed){xone::diagnostic("power_resume");stop_radio();resumed=false;}
                if(hardware_changed || (!radio_.alive() && Clock::now()>=adapter_check_)){
                    hardware_changed=false;adapter_check_=Clock::now()+std::chrono::seconds(30);
                    const auto devices=xone::adapters();
                    const auto saved=config_.value("AdapterInstanceId",std::string{});
                    auto found=devices.end();
                    for(auto d=devices.begin();d!=devices.end();++d){
                        if(_wcsicmp(d->service.c_str(),L"WinUSB") || !xone::supported_adapter_instance(d->instance))continue;
                        if(!saved.empty()){if(_stricmp(xone::utf8(d->instance).c_str(),saved.c_str())==0){found=d;break;}}
                        else {if(found!=devices.end()){found=devices.end();break;}found=d;}
                    }
                    if(found!=devices.end() && healthy(found->instance))adapter_=found->instance;else adapter_.clear();
                    if(adapter_.empty() && radio_.alive()){xone::diagnostic("adapter_unavailable",{},true);failed_radio("Adapter disconnected; waiting before radio restart");stop_radio();}
                }
                if(!radio_.alive()){
                    if(radio_.process.value){GetExitCodeProcess(radio_.process.value,&last_radio_exit_);failed_radio("Radio exited with code "+std::to_string(last_radio_exit_));stop_radio();}
                    if(adapter_.empty())state("WaitingForAdapter",retry_reason_);
                    else if(!radio_retry_.ready(Clock::now()))state("Retrying",retry_reason_);
                    else if(!fs::exists(work/L"firmware/FW_ACC_00U.bin"))state("SetupRequired","Adapter firmware is missing");else start_radio();
                }else{
                    if(radio_retry_.observe_running(Clock::now()))xone::diagnostic("radio_recovery_stable");
                    xone::service_peers(peers_,[&](Peer &p){
                        window_.pump();if(memory_.stopped() || resumed || !p.retry.ready(Clock::now()))return;
                        if(p.attach.pending || (p.port && !p.generation))detach(p); // retry failed cleanup even before RF reconnect
                        const auto s=memory_.read(static_cast<size_t>(&p-peers_.data()));
                        // An unavailable/partially observed status file is not
                        // a radio disconnect. Only an explicit snapshot may
                        // detach this slot's imported USB device.
                        service_peer_snapshot(p,s,Clock::now(),[&](Peer &peer){
                            const auto deadline=Clock::now()+std::chrono::seconds(15);
                            peer.attach.attach(peer.port,peer.model.port,[&](const std::wstring &args,bool get_port,std::string *captured){
                                return usbip(args,get_port,captured,deadline,memory_.stop_event());
                            });
                            peer.attached_at=Clock::now();peer.import.begin(s.value("SnapshotSequence",uint64_t(0)),s.value("UsbImports",uint64_t(0)),s.value("UsbClosed",uint64_t(0)),peer.attached_at,s.value("AssociationId",uint64_t(0)),GetTickCount64());
                        },[&](Peer &peer){detach(peer);},[&]{save_devices();});
                    },[&](Peer &p,const std::string &error){peer_failed(p,error,Clock::now());});
                    // Audio endpoint choice belongs to Windows and the user.
                    // Connecting another peer must not change global defaults.
                    state(std::any_of(peers_.begin(),peers_.end(),[](const auto &p){return p.ready && p.port!=0;})?"DevicesReady":"WaitingForDevices");
                }
            }catch(const std::exception &e){xone::diagnostic("host_retry",{{"error",e.what()}},true);if(!radio_.alive())failed_radio(e.what());state("Retrying",e.what());}
            HANDLE waits[3]{memory_.stop_event(),memory_.changed_event(),nullptr};DWORD count=2;if(radio_.alive())waits[count++]=radio_.process.value;
            (void)MsgWaitForMultipleObjects(count,waits,FALSE,2000,QS_ALLINPUT);
        }
        stop_radio();state("Stopped");
        if(registry_dirty_)try{write(root/L"config/paired-devices.local.json",registry_);}catch(const std::exception &e){xone::diagnostic("pairing_metadata_save_failed",{{"error",e.what()}},true);}
    }
};
int main_host(std::wstring_view argument){
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);binary_dir=fs::path(executable).parent_path();root=binary_dir.parent_path();if(root.filename()==L"outputs")root=root.parent_path();
    if(argument==L"--stop" || argument==L"--status"){
        try{
            xone::RuntimeState memory(root,false);
            if(argument==L"--stop"){
                const auto host=memory.read(xone::RuntimeState::host,100);memory.request_stop();
                if(host.contains("Process")){
                    Handle process(OpenProcess(SYNCHRONIZE,FALSE,host["Process"].value("Pid",DWORD(0))));
                    if(process.value && WaitForSingleObject(process.value,35000)!=WAIT_OBJECT_0)return 2;
                }
                return 0;
            }
            Json slots=Json::array();for(size_t n=0;n<16;++n)slots.push_back(memory.read(n,100));
            std::cout<<Json({{"Host",memory.read(xone::RuntimeState::host,100)},{"Runtime",memory.read(xone::RuntimeState::running,100)},{"Radio",memory.read(xone::RuntimeState::radio,100)},{"Slots",slots}}).dump()<<std::endl;
            return 0;
        }catch(const std::exception &){if(argument==L"--status"){std::cout<<"{}"<<std::endl;return 0;}return 3;}
    }
    xone::initialize_diagnostics("host",root/L"work/diagnostics/cpp");
    if(argument==L"--validate-config"){xone::appliance_devices(read(root/L"config/devices.local.json"));return 0;}
    const auto selected=xone::background_adapter_option(argument);
    uint64_t hash=1469598103934665603ULL;for(auto c:root.wstring()){hash^=static_cast<uint64_t>(std::towlower(c));hash*=1099511628211ULL;}
    const auto name=L"Local\\xone-headless-"+std::to_wstring(hash);Handle mutex(CreateMutexW(nullptr,TRUE,name.c_str()));if(!mutex.value)throw std::runtime_error("Cannot own background session");if(GetLastError()==ERROR_ALREADY_EXISTS)return 0;
    const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))throw std::runtime_error("Cannot initialize Windows audio");
    struct Com{~Com(){CoUninitialize();}} com;Host host(selected);host.run();ReleaseMutex(mutex.value);return 0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR arguments,int){try{return main_host(arguments?arguments:L"");}catch(const std::exception &e){xone::diagnostic("host_failed",{{"error",e.what()}},true);return 1;}}

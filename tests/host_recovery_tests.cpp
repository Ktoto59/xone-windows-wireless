// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual process/job/pipe implementation with this executable as
// its dependency. No Host instance, USB commands, adapter or window is opened.
#include "../src/background_host.cpp"
#include <source_location>
using namespace std::chrono_literals;
void require(bool value,const std::source_location location=std::source_location::current()){
    if(!value)throw std::runtime_error("Host recovery assertion at line "+std::to_string(location.line()));
}
template<class Operation> std::string expect_failure(Operation operation,const std::source_location location=std::source_location::current()){
    try{operation();}catch(const std::exception &e){return e.what();}
    require(false,location);return {};
}
std::string listing(int port=7,unsigned tcp=33400,std::string serial="XONE"){
    return "Imported USB devices\n====================\nPort "+std::to_string(port)+": device in use at high-speed\n"
        "   test product\n   -> usbip://127.0.0.1:"+std::to_string(tcp)+"/1-1\n   -> serial: "+serial+"\n   -> mode: irp\n";
}
Json snapshot(uint64_t generation=1,uint64_t association=1,bool ready=true,bool recovery=false){
    return {{"Generation",generation},{"AssociationId",association},{"Ready",ready},{"RecoveryRequired",recovery},{"RadioAssociated",ready || recovery},
        {"Pid",0x0b17},{"UsbImported",true},{"SnapshotSequence",uint64_t(100)},{"UsbImports",uint64_t(1)},{"UsbClosed",uint64_t(0)}};
}
struct PeerHarness {
    Peer peer;unsigned attaches=0,detaches=0;bool fail_attach=false,fail_detach=false;
    void step(const Json &s,Clock::time_point now){
        if(!peer.retry.ready(now))return;
        try{
            if(peer.port && !peer.generation)detach();
            service_peer_snapshot(peer,s,now,[&](Peer &p){
                ++attaches;if(fail_attach)throw std::runtime_error("injected USB attach failure");
                p.port=7;p.import.begin(0,0,0,now,s.value("AssociationId",uint64_t(0)));
            },[&](Peer &){detach();},[]{});
        }catch(const std::exception &e){peer_failed(peer,e.what(),now);}
    }
    void detach(){
        ++detaches;if(fail_detach)throw std::runtime_error("injected USB detach failure");
        peer.port=0;peer.generation=0;peer.ready=false;peer.import={};peer.retry.interrupted();
    }
};
void lifecycle_regressions(){
    const auto t=Clock::time_point{}+1h;
    PeerHarness h;
    for(uint64_t n=0;n<10;++n){
        const auto s=snapshot(n+1,n+1);h.step(s,t+n*2s);require(h.peer.port==7);
        h.step(snapshot(n+1,n+1,false),t+n*2s+1s);
        require(h.peer.port==0 && h.peer.retry.failures()==0 && h.peer.retry.ready(t+n*2s+1s));
    }
    require(h.attaches==10 && h.detaches==10);
    h.step(snapshot(11,11),t+20s);require(h.peer.port==7 && h.attaches==11 && h.peer.retry.delay()==0s);
    // Association changes win over close counters/recovery, even when the
    // composite generation collides. The next observation reattaches normally.
    auto changed=snapshot(11,12,true,true);changed["UsbClosed"]=uint64_t(5);
    h.step(changed,t+21s);require(h.peer.port==0 && h.peer.retry.failures()==0);
    h.step(snapshot(11,12),t+22s);require(h.peer.port==7);
    h.step(snapshot(12,12),t+23s);require(h.peer.port==0 && h.peer.retry.failures()==0);
    h.step(snapshot(12,12),t+24s);require(h.peer.port==7);
    h.step(snapshot(13,12,false,true),t+25s);require(h.peer.port==0 && h.peer.retry.delay()==1s);
    h.step(snapshot(13,12,false,true),t+25500ms);require(h.peer.port==0);
    h.step(snapshot(13,12,false,true),t+26s);require(h.peer.port==7);
    // A normal disconnect preserves real failure history without adding to it.
    h.step(snapshot(13,12,false),t+27s);require(h.peer.retry.failures()==1);
    h.step(snapshot(14,13),t+28s);h.step(snapshot(14,13),t+29s);
    h.step(Json::object(),t+88s); // unavailable snapshot interrupts stability
    h.step(snapshot(14,13),t+89s);h.step(snapshot(14,13),t+148s);require(h.peer.retry.failures()==1);
    h.step(snapshot(14,13),t+149s);require(h.peer.retry.failures()==0 && h.peer.error.empty());
    // Confirmed USB close and missing import retain failure backoff.
    auto closed=snapshot(14,13);closed["UsbImported"]=false;closed["UsbClosed"]=uint64_t(1);closed["SnapshotSequence"]=uint64_t(101);
    h.step(closed,t+150s);require(h.peer.port==0 && h.peer.retry.delay()==1s);
    h.step(snapshot(14,13),t+151s);
    auto absent=snapshot(14,13);absent["UsbImported"]=false;absent["UsbImports"]=uint64_t(0);
    for(uint64_t n=1;n<=3;++n){absent["SnapshotSequence"]=n;h.step(absent,t+161s+n*1s);}
    require(h.peer.port==0 && h.peer.retry.delay()==2s);
    // A failed ordinary detach is a real USB failure and keeps its record.
    PeerHarness cleanup;cleanup.step(snapshot(),t);cleanup.fail_detach=true;
    cleanup.step(snapshot(1,1,false),t+1s);require(cleanup.peer.port==7 && cleanup.peer.generation==0 && cleanup.peer.retry.delay()==1s);
    cleanup.fail_detach=false;cleanup.step(Json::object(),t+2s);require(cleanup.peer.port==0);
    PeerHarness failed,sibling;failed.fail_attach=true;sibling.step(snapshot(),t);
    auto now=t;
    for(auto expected:{1s,2s,4s,8s,16s,32s,60s,60s}){
        const auto before=failed.attaches;failed.step(snapshot(),now);
        require(failed.attaches==before+1 && failed.peer.retry.delay()==expected);
        for(auto offset=0ms;offset<std::chrono::duration_cast<std::chrono::milliseconds>(expected);offset+=250ms){
            failed.step(snapshot(),now+offset);sibling.step(snapshot(),now+offset);
            require(failed.attaches==before+1 && sibling.peer.port==7 && sibling.attaches==1 && sibling.detaches==0 && sibling.peer.retry.failures()==0);
        }
        now+=expected;
    }
    require(failed.attaches==8); // hundreds of polls cannot form an attach loop
    PeerHarness stale;stale.peer.retry.failed(t);stale.step(snapshot(),t+1s);
    stale.peer.import.begin(100,1,0,t+1s,1,1000);
    auto old=snapshot();old["PublishedTickMs"]=uint64_t(999);
    stale.step(old,t+2s);stale.step(old,t+62s);require(stale.peer.retry.failures()==1);
    old["PublishedTickMs"]=uint64_t(1001);old["SnapshotSequence"]=uint64_t(101);
    stale.step(old,t+63s);require(stale.peer.retry.failures()==1);
    old["SnapshotSequence"]=uint64_t(102);stale.step(old,t+123s);require(stale.peer.retry.failures()==0);
}
void attach_regressions(){
    using xone::UsbAttachRecovery;
    const auto foreign=listing(3,33500,"OTHER");
    for(bool created:{false,true}){
        // All exception types use the same reconciliation path; no text parsing.
        for(auto message:{"exit 1","timeout","cancelled","invalid/missing response"}){
            UsbAttachRecovery attempt;int port=0;unsigned attaches=0,detaches=0,lists=0;std::string current=foreign;
            auto run=[&](const std::wstring &args,bool get_port,std::string *output){
                if(args==L"port"){++lists;*output=current;return 0;}
                if(get_port){++attaches;if(created)current+=listing(7,33400,"").substr(listing().find("Port "));throw std::runtime_error(message);}
                require(args==L"detach -p 7");++detaches;current=foreign;return 0;
            };
            bool threw=false;try{attempt.attach(port,33400,run);}catch(const std::exception &e){threw=true;require(std::string(e.what())==message);}
            require(threw && attaches==1 && lists==2 && !attempt.pending && port==(created?7:0));
            xone::detach_owned_usb_port(port,33400,"",[&](const std::wstring &args,std::string *out){run(args,false,out);},[](const std::string &){require(false);});
            require(port==0 && detaches==(created?1u:0u));
        }
    }
    // A pre-existing identical endpoint is never adopted, regardless of serial.
    for(auto serial:{"","FOREIGN"}){
        UsbAttachRecovery attempt;int port=0;unsigned attaches=0;
        expect_failure([&]{attempt.attach(port,33400,[&](const std::wstring &args,bool,std::string *out){
            if(args==L"port")*out=listing(7,33400,serial);else ++attaches;return 7;
        });});
        require(port==0 && attaches==0 && !attempt.pending);
    }
    // A failed query retains pending ownership evidence and blocks new attach.
    UsbAttachRecovery pending;int port=0;unsigned lists=0,attaches=0;
    auto unavailable=[&](const std::wstring &args,bool,std::string *out){
        if(args==L"port"){if(++lists>1)throw std::runtime_error("inventory unavailable");*out=foreign;return 0;}
        ++attaches;throw std::runtime_error("lost result");
    };
    require(expect_failure([&]{pending.attach(port,33400,unavailable);}).starts_with("lost result; attach reconciliation:"));
    require(port==0 && pending.pending && attaches==1);
    for(unsigned n=0;n<10;++n)expect_failure([&]{pending.attach(port,33400,unavailable);});
    require(attaches==1 && lists==2);
    pending.reconcile(port,33400,[&](const std::wstring &,bool,std::string *out){*out=foreign+listing(7,33400,"").substr(listing().find("Port "));return 0;});
    require(port==7 && !pending.pending);
    // Port reuse between discovery and detach still cannot remove a foreign import.
    unsigned detaches=0;xone::detach_owned_usb_port(port,33400,"",[&](const std::wstring &args,std::string *out){
        if(args==L"port")*out=listing(7,33402,"");else ++detaches;
    },[](const std::string &){require(false);});require(port==0 && detaches==0);
    // Wrong endpoint/serial, pre-existing numbers and malformed inventories.
    for(auto after:{listing(7,33402,""),listing(7,33400,"OTHER"),listing(3,33400,"")}){
        UsbAttachRecovery attempt;unsigned calls=0;port=0;
        expect_failure([&]{attempt.attach(port,33400,[&](const std::wstring &args,bool,std::string *out){
            if(args==L"port"){*out=++calls==1?foreign:after;return 0;}throw std::runtime_error("lost result");
        });});
        require(port==0 && !attempt.pending);
    }
    for(auto invalid:{std::string("garbage"),std::string("Imported USB devices\n====\n"),std::string("Imported USB devices\nPort 7: in use\n"),listing()+listing().substr(listing().find("Port "))}){
        bool threw=false;try{(void)xone::usb_port_inventory(invalid);}catch(const std::exception &){threw=true;}require(threw);
    }
    UsbAttachRecovery ambiguous;ambiguous.pending=true;ambiguous.before=xone::usb_port_inventory(foreign);port=0;
    expect_failure([&]{ambiguous.reconcile(port,33400,[&](const std::wstring &,bool,std::string *out){*out=listing(7,33400,"")+listing(8,33400,"").substr(listing().find("Port "));return 0;});});
    require(port==0 && ambiguous.pending);
    bool rejected=false;
    try{ambiguous.reconcile(port,33400,[&](const std::wstring &,bool,std::string *out){*out=listing(7,33400,"");return 0;});}catch(const std::exception &){rejected=true;}
    require(rejected && port==0 && ambiguous.pending);
    ambiguous.reconcile(port,33400,[&](const std::wstring &,bool,std::string *out){*out=foreign;return 0;});
    require(port==0 && !ambiguous.pending);
    UsbAttachRecovery success;port=0;unsigned calls=0;
    success.attach(port,33400,[&](const std::wstring &args,bool get_port,std::string *out){
        ++calls;if(args==L"port")*out=foreign;else require(get_port);return get_port?7:0;
    });require(port==7 && calls==2 && !success.pending);
}
void attach_process_regressions(const fs::path &exe){
    // The marker represents VHCI state independent of CLI process lifetime.
    // Run the real process/pipe/timeout code, but never load a USB driver.
    const auto marker=root/(L"partial-import-"+std::to_wstring(GetCurrentProcessId())+L".txt");
    struct Remove {fs::path path;~Remove(){std::error_code ignored;fs::remove(path,ignored);}} remove{marker};
    for(auto mode:{L"hang",L"error",L"missing",L"malformed"}){
        {std::ofstream reset(marker,std::ios::trunc);}
        xone::UsbAttachRecovery attempt;int port=0;unsigned attaches=0,detaches=0;
        auto run=[&](const std::wstring &args,bool get_port,std::string *out){
            if(args==L"port"){std::ifstream in(marker);*out=std::string(std::istreambuf_iterator<char>(in),{});return 0;}
            if(get_port){++attaches;return run_usbip(exe,L"--partial-import "+quote(marker.wstring())+L" "+mode,true,nullptr,Clock::now()+500ms);}
            require(args==L"detach -p 7");++detaches;std::ofstream reset(marker,std::ios::trunc);return 0;
        };
        bool threw=false;try{attempt.attach(port,33400,run);}catch(const std::exception &){threw=true;}
        require(threw && port==7 && !attempt.pending && attaches==1);
        xone::detach_owned_usb_port(port,33400,"",[&](const std::wstring &args,std::string *out){run(args,false,out);},[](const std::string &){require(false);});
        require(port==0 && detaches==1);
    }
}
int wmain(int argc,wchar_t **argv){try{
    wchar_t name[32768]{};GetModuleFileNameW(nullptr,name,32768);const fs::path exe=name;
    root=exe.parent_path();
    if(argc>1){
        if(std::wstring_view(argv[1])==L"--partial-import"){
            if(argc!=4)return 6;
            {std::ofstream state(fs::path(argv[2]),std::ios::trunc);state<<listing(7,33400,"");if(!state)return 7;}
            const std::wstring_view mode=argv[3];
            if(mode==L"hang")Sleep(INFINITE);
            if(mode==L"malformed"){DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"invalid",7,&written,nullptr);}
            return mode==L"error"?9:0;
        }
        if(std::wstring_view(argv[1])==L"--exit")return 0;
        if(std::wstring_view(argv[1])==L"--tree" || std::wstring_view(argv[1])==L"--orphan"){
            auto command=quote(exe.wstring())+L" --hang";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
            if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))return 5;
            const auto pid=std::to_string(info.dwProcessId);DWORD written=0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),pid.data(),static_cast<DWORD>(pid.size()),&written,nullptr);
            CloseHandle(info.hThread);CloseHandle(info.hProcess);
            if(std::wstring_view(argv[1])==L"--orphan")return 0;
        }
        Sleep(INFINITE);return 0;
    }
    lifecycle_regressions();attach_regressions();attach_process_regressions(exe);
    using xone::UsbPortOwner;
    require(xone::usb_port_owner("",7,33400,"XONE")==UsbPortOwner::absent);
    // Real host attach uses no serial override. CLI metadata is empty while
    // the server's existing twenty-character USB descriptor stays unchanged.
    require(xone::usb_port_owner(listing(7,33400,""),7,33400,"")==UsbPortOwner::owned);
    require(xone::usb_port_owner(listing(7,33400,"FOREIGN"),7,33400,"")==UsbPortOwner::foreign);
    {int port=7;unsigned detaches=0;
        xone::detach_owned_usb_port(port,33400,"",[&](const std::wstring &args,std::string *text){
            if(args==L"port")*text=listing(7,33400,"");else {require(args==L"detach -p 7");++detaches;}
        },[](const std::string &){require(false);});require(port==0 && detaches==1);
    }

    require(xone::usb_port_owner(listing(),7,33400,"XONE")==UsbPortOwner::owned);
    require(xone::usb_port_owner(listing(),8,33400,"XONE")==UsbPortOwner::absent);
    require(xone::usb_port_owner(listing(7,33402),7,33400,"XONE")==UsbPortOwner::foreign);
    require(xone::usb_port_owner(listing(7,33400,"OTHER"),7,33400,"XONE")==UsbPortOwner::foreign);
    require(xone::usb_port_owner("Imported USB devices\nPort 7: in use\n",7,33400,"XONE")==UsbPortOwner::unknown);
    require(xone::usb_port_owner("garbage",7,33400,"XONE")==UsbPortOwner::unknown);
    require(xone::usb_port_owner("Imported USB devices\n====================\n",7,33400,"XONE")==UsbPortOwner::unknown);
    {auto windows=listing();for(size_t at=0;(at=windows.find('\n',at))!=std::string::npos;at+=2)windows.insert(at,1,'\r');
        require(xone::usb_port_owner(windows,7,33400,"XONE")==UsbPortOwner::owned);
    }
    for(auto output:{std::string{},listing(8),listing(7,33402),listing(7,33400,"OTHER")}){
        int port=7;unsigned detaches=0;
        xone::detach_owned_usb_port(port,33400,"XONE",[&](const std::wstring &args,std::string *text){if(args==L"port")*text=output;else ++detaches;},[](const std::string &){});
        require(port==0 && detaches==0);
    }
    for(bool verification_fails:{false,true}){
        int port=7;unsigned lists=0,errors=0;bool threw=false;
        try{xone::detach_owned_usb_port(port,33400,"XONE",[&](const std::wstring &args,std::string *text){
            if(args!=L"port")throw std::runtime_error("detach failure");
            if(++lists==2 && verification_fails)throw std::runtime_error("listing failure");
            *text=listing();
        },[&](const std::string &error){require(error=="detach failure");++errors;});}
        catch(const std::exception &e){threw=true;require(std::string(e.what()).find("detach failure")!=std::string::npos);}
        require(threw && port==7 && errors==1 && lists==2);
    }
    {int port=7;unsigned lists=0,errors=0;
        xone::detach_owned_usb_port(port,33400,"XONE",[&](const std::wstring &args,std::string *text){
            if(args!=L"port")throw std::runtime_error("EOF race");*text=++lists==1?listing():"";
        },[&](const std::string &){++errors;});require(port==0 && errors==1);
    }
    std::array<Peer,3> peers;for(size_t n=0;n<peers.size();++n){peers[n].model.name=std::to_string(n);peers[n].port=static_cast<int>(7+n);}
    unsigned closed=0,requests=0,errors=0,visited=0;
    for(unsigned repeat=0;repeat<2;++repeat){
        xone::stop_owned_radio(peers,[&]{++requests;throw std::runtime_error("request failure");},[&]{++closed;},[&](Peer &p){
            require(closed==repeat+1);++visited;if(p.port==7)throw std::runtime_error("detach failure");p.port=0;
        },[&](const std::string &,const std::string &){++errors;});
    }
    require(closed==2 && requests==2 && errors==4 && visited==6 && peers[0].port==7 && peers[1].port==0);
    unsigned healthy=0;
    for(unsigned round=0;round<100;++round)xone::service_peers(peers,[&](Peer &p){if(p.port==7)throw std::runtime_error("persistent first-slot failure");++healthy;},[](Peer &,const std::string &){});
    require(healthy==200);
    for(auto mode:{L"--tree",L"--orphan"}){
        HANDLE r=nullptr,w=nullptr;SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};require(CreatePipe(&r,&w,&security,0)!=FALSE);
        Handle reader(r),writer(w);require(SetHandleInformation(r,HANDLE_FLAG_INHERIT,0)!=FALSE);
        Child child;child.start(exe,mode,w);CloseHandle(writer.value);writer.value=nullptr;
        DWORD available=0;const auto until=Clock::now()+3s;
        while(!available && Clock::now()<until){PeekNamedPipe(r,nullptr,0,nullptr,&available,nullptr);std::this_thread::sleep_for(1ms);}
        require(available>0);char data[64]{};DWORD got=0;require(ReadFile(r,data,std::min<DWORD>(available,63),&got,nullptr)!=FALSE);
        Handle descendant(OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(std::stoul(std::string(data,got)))));require(descendant.value!=nullptr);
        Handle parent(OpenProcess(SYNCHRONIZE,FALSE,child.pid));require(parent.value!=nullptr);
        if(std::wstring_view(mode)==L"--orphan")require(WaitForSingleObject(parent.value,3000)==WAIT_OBJECT_0);
        require(child.close());require(WaitForSingleObject(parent.value,3000)==WAIT_OBJECT_0);
        require(WaitForSingleObject(descendant.value,3000)==WAIT_OBJECT_0);require(child.close());
    }
    {xone::PeerRecoveryBackoff retry,sibling;auto now=Clock::time_point{}+1h;
        for(auto expected:{1s,2s,4s,8s,16s,32s,60s,60s}){
            retry.failed(now);require(retry.delay()==expected && !retry.ready(now+expected-1ms) && retry.ready(now+expected));
            require(sibling.ready(now) && sibling.failures()==0);now+=expected;
        }
        require(!retry.observe_healthy(true,now));require(!retry.observe_healthy(true,now+59s));
        retry.interrupted();require(!retry.observe_healthy(true,now+60s));
        require(!retry.observe_healthy(false,now+119s));require(!retry.observe_healthy(true,now+120s));
        require(retry.observe_healthy(true,now+180s) && retry.failures()==0);
        retry.failed(now+181s);require(retry.delay()==1s);
    }
    {Handle cancellation(CreateEventW(nullptr,TRUE,FALSE,nullptr));require(cancellation.value!=nullptr);
        std::jthread signal([&]{std::this_thread::sleep_for(30ms);SetEvent(cancellation.value);});
        const auto start=Clock::now();bool cancelled=false;
        try{run_usbip(exe,L"--hang",false,nullptr,Clock::now()+5s,cancellation.value);}
        catch(const std::exception &e){cancelled=std::string(e.what()).find("cancelled")!=std::string::npos;}
        require(cancelled && Clock::now()-start<1s);
    }
    DWORD before=0,after=0;require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE);
    for(unsigned n=0;n<12;++n){
        bool timeout=false;try{run_usbip(exe,L"--hang",false,nullptr,Clock::now()+100ms);}catch(const std::exception &){timeout=true;}require(timeout);
        require(run_usbip(exe,L"--exit",false,nullptr,Clock::now()+3s)==0);
    }
    require(GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE);require(after<=before);
    std::cout<<"PASS host recovery: detach errors, ownership/reused ports, isolated slots, repeated stop, hung dependencies, descendant jobs and handles\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

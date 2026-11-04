// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual process/job/pipe implementation with this executable as
// its dependency. No Host instance, USB commands, adapter or window is opened.
#include "../src/background_host.cpp"
#include <source_location>
using namespace std::chrono_literals;
void require(bool value,const std::source_location location=std::source_location::current()){
    if(!value)throw std::runtime_error("Host recovery assertion at line "+std::to_string(location.line()));
}
std::string listing(int port=7,unsigned tcp=33400,std::string serial="XONE"){
    return "Imported USB devices\n====================\nPort "+std::to_string(port)+": device in use at high-speed\n"
        "   test product\n   -> usbip://127.0.0.1:"+std::to_string(tcp)+"/1-1\n   -> serial: "+serial+"\n";
}
int wmain(int argc,wchar_t **argv){try{
    wchar_t name[32768]{};GetModuleFileNameW(nullptr,name,32768);const fs::path exe=name;
    root=exe.parent_path();
    if(argc>1){
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

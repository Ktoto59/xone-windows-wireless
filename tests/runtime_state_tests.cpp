// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime_state.hpp"
#include <iostream>
using namespace xone;using namespace std::chrono_literals;
void require(bool value){if(!value)throw std::runtime_error("Memory IPC assertion failed");}
template<class F> void until(F test){const auto end=std::chrono::steady_clock::now()+3s;while(!test()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("Memory IPC wait timed out");std::this_thread::sleep_for(1ms);}}
int wmain(int argc,wchar_t **argv){try{
    if(argc==3 && std::wstring_view(argv[1])==L"--serve"){
        RuntimeState memory(argv[2],true);
        FILETIME born{},exit{},kernel{},user{};require(GetProcessTimes(GetCurrentProcess(),&born,&exit,&kernel,&user)!=FALSE);
        const auto ticks=(uint64_t(born.dwHighDateTime)<<32|born.dwLowDateTime)+504911232000000000ULL;
        const Json identity={{"Pid",GetCurrentProcessId()},{"StartTicks",ticks}};
        memory.publish(RuntimeState::host,Json({{"Phase","Testing"},{"Process",identity}}).dump());
        memory.publish(RuntimeState::running,Json({{"Running",true},{"Worker",identity}}).dump());
        until([&]{return memory.read(RuntimeState::host).value("Phase",std::string{})=="Testing";});
        std::cout<<"READY"<<std::endl;return WaitForSingleObject(memory.stop_event(),15000)==WAIT_OBJECT_0?0:1;
    }
    if(argc==3 && std::wstring_view(argv[1])==L"--client"){
        RuntimeState memory(argv[2],false);memory.publish(0,"{\"Generation\":7,\"Ready\":true}",true);
        require(WaitForSingleObject(memory.radio_stop_event(),3000)==WAIT_OBJECT_0);
        require(!memory.stopped());return 0;
    }
    const auto root=std::filesystem::absolute(L"work/memory-state-test-"+std::to_wstring(GetCurrentProcessId()));
    require(!std::filesystem::exists(root));
    {
        RuntimeState owner(root,true),reader(root,false);owner.publish(RuntimeState::host,"{\"Phase\":\"Ready\"}",true);
        until([&]{return reader.read(RuntimeState::host).value("Phase",std::string{})=="Ready";});
        std::mutex mutex;std::condition_variable wake;bool held=false,release=false;
        std::thread slow([&]{HANDLE lock=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(RuntimeState::name(root)+L"-lock").c_str());
            require(lock && WaitForSingleObject(lock,1000)==WAIT_OBJECT_0);{std::unique_lock guard(mutex);held=true;wake.notify_all();wake.wait(guard,[&]{return release;});}ReleaseMutex(lock);CloseHandle(lock);});
        {std::unique_lock lock(mutex);require(wake.wait_for(lock,1s,[&]{return held;}));}
        const auto before=std::chrono::steady_clock::now();for(unsigned n=0;n<1000;++n)owner.publish(1,Json({{"Counter",n}}).dump(),true);
        require(std::chrono::steady_clock::now()-before<250ms);
        {std::lock_guard lock(mutex);release=true;}wake.notify_all();slow.join();
        until([&]{return reader.read(1).value("Counter",0u)==999;});
        bool rejected=false;try{owner.publish(0,std::string(65536,'x'));}catch(const std::runtime_error &){rejected=true;}require(rejected);
        wchar_t executable[32768]{};require(GetModuleFileNameW(nullptr,executable,32768)!=0);
        std::wstring command=L"\""+std::wstring(executable)+L"\" --client \""+root.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
        require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE);
        CloseHandle(child.hThread);until([&]{return reader.read(0).value("Generation",0u)==7;});
        owner.request_radio_stop();require(WaitForSingleObject(child.hProcess,3000)==WAIT_OBJECT_0);DWORD exit=1;require(GetExitCodeProcess(child.hProcess,&exit)!=FALSE && exit==0);CloseHandle(child.hProcess);
        owner.request_stop();require(reader.stopped());
        RuntimeState isolated(root.wstring()+L"-other",true);require(!isolated.stopped() && isolated.read(0).empty());
    }
    bool missing=false;try{RuntimeState reader(root,false);}catch(const std::runtime_error &){missing=true;}require(missing);
    require(!std::filesystem::exists(root));
    std::cout<<"PASS memory IPC: cross-process state/events, coalescing, slow observer isolation, bounds, lifetime and no files\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "diagnostic_log.hpp"
#include <future>
#include <atomic>
#include <iostream>
using namespace xone;using namespace std::chrono_literals;
void require(bool value){if(!value)throw std::runtime_error("Diagnostic journal assertion failed");}
int main(int argc,char **argv){if(argc!=2)return 2;try{
    const auto directory=std::filesystem::path(argv[1]);
    std::mutex mutex;std::condition_variable gate;bool release=false,entered=false;std::vector<Json> anomalies;
    DiagnosticJournal::Limits limits;limits.capacity=2;
    {
        DiagnosticJournal journal(directory,"bounded",limits,[&](bool anomaly,const Json &row){
            std::unique_lock lock(mutex);entered=true;gate.notify_all();gate.wait(lock,[&]{return release;});if(anomaly)anomalies.push_back(row);
        });
        journal.event("block");{std::unique_lock lock(mutex);require(gate.wait_for(lock,1s,[&]{return entered;}));}
        for(unsigned n=0;n<100;++n)journal.event("ordinary",{{"n",n}});
        journal.event("usb_error",{{"win32_error",31}},true);
        {std::lock_guard lock(mutex);release=true;}gate.notify_all();
    }
    require(std::any_of(anomalies.begin(),anomalies.end(),[](const auto &r){return r["operation"]=="usb_error";}));
    require(std::any_of(anomalies.begin(),anomalies.end(),[](const auto &r){return r["operation"]=="trace_loss" && r["fields"]["dropped"].template get<unsigned>()>0;}));
    {
        limits.capacity=2048;limits.max_bytes=512;limits.backups=2;
        DiagnosticJournal journal(directory,"rotation",limits);
        for(unsigned n=0;n<40;++n)journal.event("metadata",{{"seq",n},{"bytes",192}},true);
    }
    size_t count=0;for(const auto &file:std::filesystem::directory_iterator(directory)){
        if(file.path().filename().string().find("rotation")==std::string::npos)continue;++count;
        std::ifstream in(file.path());std::string line;while(std::getline(in,line))require(!Json::parse(line).is_discarded());
    }require(count<=7 && count>=3);
    {
        std::atomic_uint attempted=0;
        DiagnosticJournal journal(directory,"failure",limits,[&](bool,const Json &){++attempted;throw std::runtime_error("fake disk failure");});
        journal.event("write_failure",{},true);
        const auto end=std::chrono::steady_clock::now()+1s;while(!attempted && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);
        require(attempted>0);
    }
    {
        struct Gate {std::mutex mutex;std::condition_variable wake;bool entered=false,release=false,returned=false;};
        auto blocked=std::make_shared<Gate>();limits.shutdown=20ms;
        auto journal=std::make_unique<DiagnosticJournal>(directory,"slow",limits,[blocked](bool,const Json &){
            std::unique_lock lock(blocked->mutex);blocked->entered=true;blocked->wake.notify_all();
            blocked->wake.wait(lock,[&]{return blocked->release;});blocked->returned=true;blocked->wake.notify_all();
        });
        journal->event("blocked_writer");
        {std::unique_lock lock(blocked->mutex);require(blocked->wake.wait_for(lock,1s,[&]{return blocked->entered;}));}
        const auto started=std::chrono::steady_clock::now();journal.reset();require(std::chrono::steady_clock::now()-started<500ms);
        {std::lock_guard lock(blocked->mutex);blocked->release=true;}blocked->wake.notify_all();
        {std::unique_lock lock(blocked->mutex);require(blocked->wake.wait_for(lock,1s,[&]{return blocked->returned;}));}
    }
    std::cout<<"PASS journal: independent priority queue, overflow report, rotation, JSON and IO failure isolation\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_device_route.hpp"
#include "auto_gip_router.hpp"
#include "radio_event_queue.hpp"
#include <iostream>
using namespace xone;using namespace std::chrono_literals;
void require(bool ok){if(!ok)throw std::runtime_error("Native route recovery regression");}
struct Observation {RawGipOutput::Time arrived{};uint8_t kind=0;unsigned events=0;};
class FakePeer final:public RawGipOutput {
    std::shared_ptr<Observation> seen_;
public:
    explicit FakePeer(std::shared_ptr<Observation> seen):seen_(std::move(seen)){}
    bool target(std::span<const uint8_t>)const override{return true;}
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data)override{event_at(kind,wcid,data,std::chrono::steady_clock::now());}
    void event_at(uint8_t kind,uint8_t,std::span<const uint8_t>,Time arrived)override{seen_->arrived=arrived;seen_->kind=kind;++seen_->events;}
    std::vector<RawGipTransmit> poll()override{return {};}
    void complete(uint8_t,uint32_t,int)override{}
};
int main(){try{
    const auto start=std::chrono::steady_clock::time_point{}+100s;auto now=start;
    unsigned factories=0;auto seen=std::make_shared<Observation>();
    AutoGipRouter router(2,[&](size_t,const Bytes &mac){
        return std::make_unique<NativeDeviceRoute>(uint16_t(33400),mac,L"",false,NativeUsbOutput::StateSink{},[&]{return now;},
            [&](uint16_t pid,bool audio){require(pid==0x0b32 && audio);++factories;return std::make_unique<FakePeer>(seen);});
    });
    const Bytes a{2,0,0,0,0,1},b{2,0,0,0,0,2};
    router.event_at(1,1,a,start);router.event_at(1,2,b,start);require(factories==0 && router.poll().empty());
    now=start+1s;Bytes body(28);body[8]=0x5e;body[9]=4;body[10]=0x32;body[11]=0x0b;
    router.event_at(2,1,encode_gip({2,32,1,0,body}),now);require(factories==1);
    RadioEventQueue events;RadioEvent event;event.kind=RadioEvent::Kind::gip;event.wcid=1;event.payload=encode_gip({0x60,32,1,0,{}});
    events.push(event,start+2s);now=start+2s+20ms;auto batch=events.drain(now);require(batch.size()==1 && batch[0].arrived==start+2s);
    router.event_at(4,1,batch[0].payload,batch[0].arrived);require(seen->arrived==start+2s && seen->kind==4);
    now=start+4999ms;require(router.poll().empty() && router.faults().empty());
    now=start+5s;require(router.poll().empty());auto faults=router.faults();
    require(faults.size()==1 && faults[0].wcid==2 && router.active()==1 && factories==1);
    // The healthy sibling remains alive; no socket/default native factory was opened.
    router.event_at(4,1,{},now);require(seen->arrived==now);
    router.event(3,2);router.event(1,2,b);now+=1s;require(router.poll().empty() && router.faults().empty());
    std::cout<<"PASS: timestamp propagation, five-second announce watchdog, retry rearm and sibling isolation without USB/sockets\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

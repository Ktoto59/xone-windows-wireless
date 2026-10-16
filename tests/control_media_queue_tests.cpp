// SPDX-License-Identifier: GPL-2.0-or-later
#include "control_media_queue.hpp"
#include "radio_event_queue.hpp"
#include "raw_gip.hpp"
#include "gip_link.hpp"
#include "radio_ipc_queue.hpp"
#include <iostream>
using namespace std::chrono_literals;
static void check(bool value,const char *text){if(!value)throw std::runtime_error(text);}
int main(){try{
    using Queue=xone::ControlMediaQueue<int>;const Queue::Clock::time_point now{};
    Queue queue(128,64,4);
    for(int n=0;n<128;++n)queue.push(n,false,1,now);
    for(int n=0;n<10000;++n)queue.push(10000+n,true,1,now);
    queue.push(20000,true,2,now);
    check(queue.reliable_size()==128 && queue.media_size()==5 && queue.dropped_media()==9996,"Audio displaced reliable traffic or another peer");
    bool overflow=false;try{queue.push(999,false,1,now);}catch(const std::overflow_error &){overflow=true;}
    check(overflow && queue.reliable_size()==128,"Control saturation was silent");
    auto first=queue.drain(now,64),second=queue.drain(now,64);
    for(int n=0;n<64;++n){check(first[n]==n,"Reliable FIFO/order lost");check(second[n]==n+64,"Control barrier was overtaken");}
    check(queue.drain(now+33ms).empty() && queue.dropped_media()==10001,"Expired media retained");
    queue.push(42,false,1,now);check(queue.drain(now+10s).front()==42,"Service data expired with audio");

    xone::RadioEventQueue rx;xone::RadioEvent media;media.kind=xone::RadioEvent::Kind::gip;media.wcid=1;media.address={1,2,3,4,5,6};
    media.payload=xone::encode_gip({0x60,32,1,0,xone::Bytes(386)});
    check(xone::radio_audio_only(media),"PCM was classified as reliable");
    for(unsigned n=0;n<100;++n)rx.push(media,now);
    auto control=media;control.payload=xone::encode_gip({8,32,1,0,{1,2,3}});rx.push(control,now);
    auto batch=rx.drain(now);check(batch.size()==5 && batch.front().payload==control.payload,"Radio control stuck behind microphone flood");
    auto mixed=media;mixed.payload.insert(mixed.payload.end(),control.payload.begin(),control.payload.end());
    check(!xone::radio_audio_only(mixed),"Mixed control/audio transfer became droppable");
    mixed.payload={0xff};check(!xone::radio_audio_only(mixed),"Unknown packet became droppable");
    rx.push(media,now);auto lost=control;lost.kind=xone::RadioEvent::Kind::lost;rx.push(lost,now);
    batch=rx.drain(now);check(batch.size()==1 && batch[0].kind==xone::RadioEvent::Kind::lost,"Old audio survived disconnect barrier");
    rx.push(media,now);auto association=media;association.kind=xone::RadioEvent::Kind::association;rx.push(association,now);
    check(rx.drain(now).size()==1,"Old audio survived reassociation");

    xone::RadioIpcQueue ipc;ipc.push({4,1,2},true,now);ipc.push({17},false,now);
    check(ipc.next(now)[0]==17,"IPC service did not bypass unsent audio");ipc.advance(1);
    check(ipc.next(now)[0]==4,"Unexpired IPC audio was lost");ipc.advance(1);ipc.push({18},false,now);
    check(ipc.next(now+1s).size()==2,"Partially sent audio frame was corrupted");ipc.advance(2);
    check(ipc.next(now+1s)[0]==18,"IPC control lost behind partial frame");
    std::vector<xone::RawGipTransmit> tx{{1,1,{},true},{2,2,{},false},{1,3,{},false},{2,4,{},true}};
    xone::prioritize_control(tx);
    check(tx[0].token==2 && tx[1].token==3 && tx[2].token==1 && tx[3].token==4,"TX priority broke per-class FIFO/fairness");
    std::cout<<"Control/media isolation, saturation, expiry, lifecycle, mixed-frame and partial-TCP tests passed\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

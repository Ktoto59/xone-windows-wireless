// SPDX-License-Identifier: GPL-2.0-or-later
#include "auto_gip_router.hpp"
using namespace xone;
struct State{Bytes mac;std::vector<RawGipTransmit> requests;std::vector<std::pair<uint32_t,int>> results;bool fail=false,fail_event=false,fail_complete=false,closed=false;unsigned events=0;};
struct Peer final:RawGipOutput{
    std::shared_ptr<State> state;
    explicit Peer(std::shared_ptr<State> s):state(std::move(s)){}
    ~Peer()override{state->closed=true;}
    bool target(std::span<const uint8_t> m)const override{return std::equal(m.begin(),m.end(),state->mac.begin(),state->mac.end());}
    void event(uint8_t,uint8_t,std::span<const uint8_t>)override{if(state->fail_event)throw std::runtime_error("event failure");++state->events;}
    std::vector<RawGipTransmit> poll()override{if(state->fail)throw std::runtime_error("isolated failure");auto out=std::move(state->requests);state->requests.clear();return out;}
    void complete(uint8_t,uint32_t token,int status)override{if(state->fail_complete)throw std::runtime_error("completion failure");state->results.emplace_back(token,status);}
};
void require(bool ok){if(!ok)throw std::runtime_error("Automatic router regression");}
int main(){
    try{
        std::vector<std::shared_ptr<State>> states;
        AutoGipRouter router(2,[&](size_t,const Bytes &mac){auto s=std::make_shared<State>();s->mac=mac;states.push_back(s);return std::make_unique<Peer>(s);});
        const Bytes a{2,1,2,3,4,5},b{2,1,2,3,4,6},c{2,1,2,3,4,7};
        router.event(1,1,a);router.event(1,2,b);
        require(states.size()==2 && router.active()==2);
        // Same model is deliberately irrelevant to routing: instances own
        // different connections, WCIDs, queues and equal completion tokens.
        states[0]->requests={{1,1,{}},{1,2,{}}};states[1]->requests={{2,1,{}}};
        const auto batch=router.poll();require(batch.size()==3 && batch[0].wcid==1 && batch[1].wcid==2 && batch[2].wcid==1);
        router.complete(1,1,0);router.complete(2,1,-71);
        require(states[0]->results.back().second==0 && states[1]->results.back().second==-71);
        router.event(1,3,c);require(router.active()==2 && router.failures()==1);
        auto faults=router.faults();require(faults.size()==1 && faults[0].wcid==3 && router.faults().empty());
        states[0]->requests={{1,99,{}}};router.event(3,1);
        require(states[0]->closed && !states[1]->closed);
        router.event(1,1,c);require(states.size()==3 && router.active()==2);
        require(router.poll().empty()); // Old connection's queued token 99 died.
        states[2]->requests={{2,7,{}},{1,8,{}}};const auto valid=router.poll();
        require(valid.size()==1 && valid[0].token==8 && states[2]->results.back()==std::pair<uint32_t,int>{7,-19});
        states[2]->fail=true;states[1]->requests={{2,55,{}}};
        const auto sibling=router.poll();require(sibling.size()==1 && sibling[0].wcid==2 && router.active()==1 && router.failures()==2);
        faults=router.faults();require(faults.size()==1 && faults[0].wcid==1 && !faults[0].error.empty());
        router.event(3,1);router.event(3,2);require(router.active()==0);
        router.event(1,1,a);states.back()->fail_event=true;router.event(2,1,Bytes{3,32,1,0});
        faults=router.faults();require(faults.size()==1 && faults[0].wcid==1 && router.active()==0);
        router.event(3,1);router.event(1,1,a);states.back()->fail_complete=true;router.complete(1,10,0);
        // A late RF lost invalidates the fault before the WCID is reused.
        router.event(3,1);router.event(1,1,b);require(router.faults().empty() && router.active()==1);
        std::cout<<"Automatic router: instance isolation, slot reuse and peer failure passed\n";return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

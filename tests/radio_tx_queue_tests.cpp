// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio_tx_queue.hpp"
#include <future>
#include <iostream>
#include <atomic>
using namespace xone;using namespace std::chrono_literals;
void require(bool value){if(!value)throw std::runtime_error("Radio TX assertion failed");}
RadioTxQueue::Work work(uint32_t token,bool audio=false,uint8_t peer=1,uint64_t epoch=1){return {peer,token,epoch,audio,RadioTxQueue::Clock::now()+1s,{{static_cast<uint8_t>(token)}}};}
template<class F> void until(F test){const auto end=RadioTxQueue::Clock::now()+2s;while(!test()){if(RadioTxQueue::Clock::now()>end)throw std::runtime_error("Radio TX wait timed out");std::this_thread::sleep_for(1ms);}}
int main(){try{
    {
        std::mutex mutex;std::condition_variable changed;bool entered=false,release=false;std::vector<int> sent;
        RadioTxQueue queue([&](std::span<const uint8_t> data,std::stop_token,RadioTxQueue::Clock::time_point deadline){
            require((data[0]==2)==(deadline!=RadioTxQueue::Clock::time_point::max()));
            std::unique_lock lock(mutex);sent.push_back(data[0]);if(data[0]==1){entered=true;changed.notify_all();changed.wait(lock,[&]{return release;});}return 0;
        });
        queue.push(work(1));{std::unique_lock lock(mutex);require(changed.wait_for(lock,1s,[&]{return entered;}));}
        require(queue.poll().empty()); // An accepted request is not USB success.
        const auto before=RadioTxQueue::Clock::now();
        require(queue.push(work(2,true)));require(queue.push(work(3)));auto expired=work(4,true);expired.deadline=RadioTxQueue::Clock::now()-1ms;require(queue.push(std::move(expired)));
        require(RadioTxQueue::Clock::now()-before<100ms); // Slow USB cannot hold producers.
        {std::lock_guard lock(mutex);release=true;}changed.notify_all();
        std::vector<RadioTxQueue::Result> results;until([&]{auto r=queue.poll();results.insert(results.end(),r.begin(),r.end());return results.size()==4;});
        require((sent==std::vector<int>{1,3,2}));
        require(std::any_of(results.begin(),results.end(),[](const auto &r){return r.token==4 && r.status==-18 && !r.packets;}));
    }
    {
        std::atomic_bool entered=false;
        RadioTxQueue queue([&](std::span<const uint8_t>,std::stop_token stop,RadioTxQueue::Clock::time_point){entered=true;while(!stop.stop_requested())std::this_thread::sleep_for(1ms);return -19;});
        queue.push(work(1,false,1,10));until([&]{return entered.load();});queue.push(work(2,true,1,10));
        queue.cancel(1,10);
        std::vector<RadioTxQueue::Result> result;until([&]{auto r=queue.poll();result.insert(result.end(),r.begin(),r.end());return result.size()==2;});
        require(std::all_of(result.begin(),result.end(),[](const auto &r){return r.status==-19 && r.epoch==10;}));
        const auto before=RadioTxQueue::Clock::now();queue.stop();require(RadioTxQueue::Clock::now()-before<500ms);
    }
    {
        std::atomic_bool entered=false;
        RadioTxQueue queue([&](std::span<const uint8_t>,std::stop_token stop,RadioTxQueue::Clock::time_point){entered=true;while(!stop.stop_requested())std::this_thread::sleep_for(1ms);return -19;});
        queue.push(work(1));until([&]{return entered.load();});
        for(unsigned n=0;n<4;++n)require(queue.push(work(2+n,true)));
        require(!queue.push(work(6,true)));
        for(unsigned n=0;n<255;++n)require(queue.push(work(7+n)));
        bool overflow=false;try{queue.push(work(300));}catch(const std::overflow_error &){overflow=true;}require(overflow);
        queue.stop();require(queue.poll().size()==260);
    }
    {
        std::atomic_uint entered=0;std::atomic_bool release=false;std::stop_source consumer;
        RadioTxQueue queue([&](std::span<const uint8_t>,std::stop_token stop,RadioTxQueue::Clock::time_point){++entered;while(!release && !stop.stop_requested())std::this_thread::sleep_for(1ms);return stop.stop_requested()?-19:0;});
        auto active=work(1);active.cancellation=consumer.get_token();queue.push(std::move(active));until([&]{return entered.load()==1;});
        auto waiting=work(2);waiting.cancellation=consumer.get_token();queue.push(std::move(waiting));consumer.request_stop();
        std::vector<RadioTxQueue::Result> results;until([&]{auto r=queue.poll();results.insert(results.end(),r.begin(),r.end());return results.size()==2;});
        require(entered==1 && std::all_of(results.begin(),results.end(),[](const auto &r){return r.status==-19;}));
    }
    std::cout<<"PASS bounded asynchronous radio TX: real completions, control priority, expiry, cancellation, backpressure and shutdown\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

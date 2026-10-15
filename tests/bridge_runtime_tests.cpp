// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio_ipc_queue.hpp"
#include "runtime_files.hpp"
#include <future>
#include <iostream>

void require(bool value){if(!value)throw std::runtime_error("Bridge fault regression failed");}
int main(){
    try{
        using namespace std::chrono_literals;
        const auto now=xone::RadioIpcQueue::Clock::now();
        xone::RadioIpcQueue burst;
        burst.push({16},false,now);
        for(unsigned i=0;i<270;++i)burst.push({4,static_cast<uint8_t>(i)},true,now);
        require(burst.size()==5 && burst.dropped_audio()==266);
        require(burst.next(now)[0]==16);burst.advance(1);
        require(burst.next(now+500ms).empty());
        require(burst.dropped_audio()==270);
        burst.push({17},false,now+500ms);
        require(burst.next(now+500ms)[0]==17);

        xone::RadioIpcQueue partial;
        partial.push({4,1,2,3},true,now);partial.advance(2);
        partial.push({4,9,9},true,now);partial.push({17,5},false,now);
        const auto remaining=partial.next(now+500ms);
        require(remaining.size()==2 && remaining[0]==2 && remaining[1]==3);
        partial.advance(2);
        require(partial.next(now+500ms)[0]==17 && partial.dropped_audio()==1);

        std::promise<void> entered,release;
        const auto unblock=release.get_future().share();
        std::vector<std::string> written;
        {
            xone::RuntimeFiles files([&](const std::string &s){
                if(s=="first"){entered.set_value();unblock.wait();}
                written.push_back(s);
            },[]{return false;});
            files.publish("first");
            if(entered.get_future().wait_for(1s)!=std::future_status::ready){release.set_value();require(false);}
            for(unsigned i=0;i<1000;++i)files.publish(std::to_string(i));
            // Successful publication while the writer is still blocked proves
            // it does not wait for disk completion or build an I/O backlog.
            release.set_value();
        }
        require(written.size()==2 && written.back()=="999");
        {
            std::promise<void> failed;
            xone::RuntimeFiles files([&](const std::string &){failed.set_value();throw std::runtime_error("disk stalled/failed");},[]{return true;});
            files.publish("snapshot");
            require(failed.get_future().wait_for(1s)==std::future_status::ready);
            for(unsigned i=0;i<100 && !files.stop_requested();++i)std::this_thread::sleep_for(1ms);
            require(files.stop_requested() && files.io_errors()==1);
        }
        std::cout<<"Bridge runtime fault tests: 4 passed\n";
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

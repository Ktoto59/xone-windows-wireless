// SPDX-License-Identifier: GPL-2.0-or-later
#include "latest_snapshot.hpp"
#include <future>
#include <iostream>
#include <vector>
#include <stdexcept>
int main(){
    std::promise<void> started,release;
    auto gate=release.get_future().share();auto waiting=started.get_future();
    std::vector<std::string> written;
    {
        xone::LatestSnapshot publisher([&](const std::string &text){
            written.push_back(text);
            if(text=="first"){started.set_value();gate.wait();throw std::runtime_error("disk unavailable");}
        });
        publisher.publish("first");
        if(waiting.wait_for(std::chrono::seconds(2))!=std::future_status::ready){release.set_value();return 1;}
        for(unsigned i=0;i<10000;++i)publisher.publish(std::to_string(i));
        publisher.publish("stopped");release.set_value();
    }
    if(written!=std::vector<std::string>{"first","stopped"})return 2;
    std::cout<<"PASS: blocked status IO is bounded, coalesced, nonfatal, and drains final state\n";
}

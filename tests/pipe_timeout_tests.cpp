// SPDX-License-Identifier: GPL-2.0-or-later
#include "pipe_timeouts.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

void require(bool condition){if(!condition)throw std::runtime_error("Pipe timeout contract failed");}
int main(){
    try{
        std::vector<std::pair<uint8_t,unsigned>> policies;
        bool fail=false;
        auto apply=[&](uint8_t pipe,unsigned timeout){policies.emplace_back(pipe,timeout);return !fail;};
        xone::PipeTimeouts connection;
        require(connection.set(0x84,250,apply));
        require(connection.set(0x85,250,apply));
        require(connection.set(0x04,250,apply));
        require(connection.set(0x04,1000,apply));
        for(unsigned i=0;i<10000;++i)require(connection.set(0x04,1000,apply));
        require(policies.size()==4); // Packet count no longer drives policy calls.
        require(connection.set(0,1000,apply));
        require(connection.set(0x04,0,apply)); // Zero retains unlimited timeout semantics.
        require(connection.set(0,1000,apply));
        require(policies.size()==6 && policies.back()==std::pair<uint8_t,unsigned>{uint8_t(0x04),0u});
        fail=true;
        require(!connection.set(0x04,500,apply));
        require(!connection.set(0x04,500,apply)); // A failed policy is never cached.
        fail=false;
        require(connection.set(0x04,0,apply)); // Restore even the old value after failure.
        require(policies.size()==9 && policies.back()==std::pair<uint8_t,unsigned>{uint8_t(0x04),0u});
        xone::PipeTimeouts reconnected;
        require(reconnected.set(0x04,0,apply)); // Fresh handles do not inherit cache.
        require(policies.size()==10);
        std::cout<<"PASS WinUSB timeout reuse, changes, zero, failure retry, endpoint and handle isolation\n";
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

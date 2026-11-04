// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <exception>
namespace xone {
// Cleanup is best effort per resource. In particular, USBip must never stand
// between requesting radio shutdown and closing its kill-on-close job.
template<class Peers,class Request,class Close,class Detach,class Error>
void stop_owned_radio(Peers &peers,Request request,Close close,Detach detach,Error error){
    try{request();}catch(const std::exception &e){error("radio-stop",e.what());}
    try{close();}catch(const std::exception &e){error("radio-close",e.what());}
    for(auto &peer:peers)try{detach(peer);}catch(const std::exception &e){error(peer.model.name,e.what());}
}
template<class Peers,class Service,class Error>
void service_peers(Peers &peers,Service service,Error error){
    for(auto &peer:peers)try{service(peer);}catch(const std::exception &e){error(peer,e.what());}
}
}

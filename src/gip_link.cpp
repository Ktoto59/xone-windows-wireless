// SPDX-License-Identifier: GPL-2.0-or-later
// Based on xone bus/protocol.c, Copyright (C) 2021 Severin von Wnuck-Lipinski.
#include "gip_link.hpp"
#include <stdexcept>
#include <algorithm>
namespace xone {
static void put_var(std::vector<uint8_t> &out,uint32_t value) {
    do { auto b=static_cast<uint8_t>(value&127); value>>=7; out.push_back(static_cast<uint8_t>(b|(value?128:0))); } while(value);
}
std::vector<uint8_t> encode_gip(const GipPacket &p) {
    if (p.payload.size()>65535 || p.chunk_offset>65535) throw std::runtime_error("GIP packet too large");
    std::vector<uint8_t> out{p.command,p.options,p.sequence};
    put_var(out,static_cast<uint32_t>(p.payload.size()));
    std::vector<uint8_t> offset;
    if (p.options&128) put_var(offset,p.chunk_offset);
    if ((out.size()+offset.size())%2) { out.back()|=128; out.push_back(0); }
    out.insert(out.end(),offset.begin(),offset.end()); out.insert(out.end(),p.payload.begin(),p.payload.end()); return out;
}
GipLink::GipLink(uint8_t id,std::function<void(const GipPacket &)> tx,std::function<void(const GipPacket &)> dispatch):id_(id),transmit_(std::move(tx)),dispatch_(std::move(dispatch)) {
    if (id>15) throw std::runtime_error("Invalid GIP client ID");
}
void GipLink::send(uint8_t command,std::span<const uint8_t> data,bool ack,bool internal) {
    if (data.size()>65535) throw std::runtime_error("GIP transfer too large");
    const uint8_t seq=sequence_++; if (!sequence_) sequence_=1;
    GipPacket p{command,static_cast<uint8_t>(id_|(internal?32:0)|(ack?16:0)),seq,0,{data.begin(),data.end()}};
    if (data.size()<=58) { transmit_(p); return; }
    if (outgoing_) throw std::runtime_error("GIP chunk transfer already pending");
    Transfer t{p,p.payload,{},false};
    t.packet.options=static_cast<uint8_t>(id_|(internal?32:0)|128); t.packet.payload.clear();
    p.options|=16|64|128; p.chunk_offset=static_cast<uint32_t>(data.size()); p.payload.resize(58);
    outgoing_=std::move(t); transmit_(p);
}
void GipLink::receive(GipPacket p) {
    if ((p.options&15)!=id_) throw std::runtime_error("GIP client mismatch");
    if ((p.options&64) && !(p.options&128)) throw std::runtime_error("Chunk start without chunk flag");
    if (p.options&64) {
        if (!p.chunk_offset || p.chunk_offset>65535) throw std::runtime_error("Invalid GIP chunk total");
        incoming_=Transfer{p,std::vector<uint8_t>(p.chunk_offset),std::vector<uint8_t>(p.chunk_offset),false}; p.chunk_offset=0;
    }
    if (p.options&128) {
        if (!incoming_) { if (p.payload.empty()) return; throw std::runtime_error("Chunk before start"); }
        auto &t=*incoming_;
        if (p.command!=t.packet.command || p.sequence!=t.packet.sequence || p.chunk_offset>t.data.size() || p.payload.size()>t.data.size()-p.chunk_offset)
            throw std::runtime_error("Conflicting GIP chunk");
        std::copy(p.payload.begin(),p.payload.end(),t.data.begin()+p.chunk_offset);
        std::fill(t.received.begin()+p.chunk_offset,t.received.begin()+p.chunk_offset+p.payload.size(),uint8_t(1));
    }
    if (p.options&16) {
        const size_t length=p.chunk_offset+p.payload.size(),remaining=(p.options&128)?incoming_->data.size()-length:0;
        GipPacket ack{1,static_cast<uint8_t>(32|id_),p.sequence,0,{0,p.command,static_cast<uint8_t>(32|id_),static_cast<uint8_t>(length),static_cast<uint8_t>(length>>8),0,0,static_cast<uint8_t>(remaining),static_cast<uint8_t>(remaining>>8)}};
        transmit_(ack);
    }
    if (p.options&128) {
        if (!p.payload.empty()) return;
        auto t=std::move(*incoming_); incoming_.reset();
        if (p.chunk_offset!=t.data.size() || std::find(t.received.begin(),t.received.end(),0)!=t.received.end()) throw std::runtime_error("Incomplete GIP transfer");
        p.options&=~uint8_t(128|64|16); p.payload=std::move(t.data); dispatch_(p); return;
    }
    if (p.command==1 && (p.options&32)) {
        if (p.payload.size()!=9) throw std::runtime_error("Invalid GIP ACK length");
        if (!outgoing_ || p.payload[1]!=outgoing_->packet.command || p.sequence!=outgoing_->packet.sequence) return;
        auto &t=*outgoing_; const size_t length=p.payload[3]|(size_t(p.payload[4])<<8);
        if (length==58 && !t.sent) {
            t.sent=true;
            for (size_t off=58;off<t.data.size();off+=58) {
                auto part=t.packet; part.chunk_offset=static_cast<uint32_t>(off);
                const size_t n=std::min(size_t(58),t.data.size()-off);
                part.payload.assign(t.data.begin()+off,t.data.begin()+off+n); if (off+n==t.data.size()) part.options|=16;
                transmit_(part);
            }
        } else if (length==t.data.size() && t.sent) {
            auto end=t.packet; end.chunk_offset=static_cast<uint32_t>(t.data.size()); outgoing_.reset(); transmit_(end);
        }
        return;
    }
    dispatch_(p);
}
}

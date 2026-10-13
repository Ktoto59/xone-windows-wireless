// SPDX-License-Identifier: GPL-2.0-or-later
// Based on wire layouts in xone/bus/protocol.c, transport/dongle.c, driver/gamepad.c.
// Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
#include "protocol.hpp"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <array>

namespace xone {
static uint16_t le16(std::span<const uint8_t> b,size_t i) {
    if (i>b.size() || b.size()-i<2) throw std::runtime_error("Truncated 16-bit field");
    return static_cast<uint16_t>(b[i] | (unsigned(b[i+1])<<8));
}
static uint32_t le32(std::span<const uint8_t> b,size_t i) {
    return uint32_t(le16(b,i)) | (uint32_t(le16(b,i+2))<<16);
}
FirmwareInfo validate_firmware(std::span<const uint8_t> bytes) {
    if (bytes.size()<32) throw std::runtime_error("Truncated MT76 firmware header");
    FirmwareInfo info{le32(bytes,0),le32(bytes,4),le16(bytes,8),le16(bytes,10),{}};
    const uint64_t expected=uint64_t(32)+info.ilm+info.dlm;
    if (!info.ilm || !info.dlm || expected != bytes.size() || expected>4*1024*1024)
        throw std::runtime_error("Invalid MT76 firmware segment lengths");
    for (size_t i=16;i<32 && bytes[i];++i) {
        if (bytes[i]<32 || bytes[i]>126) throw std::runtime_error("Invalid firmware build timestamp");
        info.timestamp.push_back(static_cast<char>(bytes[i]));
    }
    return info;
}
static uint32_t varint(std::span<const uint8_t> bytes,size_t &offset) {
    uint32_t value=0;
    // xone uses at most four 7-bit groups, including an optional zero group
    // to make the complete GIP header even. Incomplete groups are rejected.
    for (unsigned i=0;i<4;++i) {
        if (offset>=bytes.size()) throw std::runtime_error("Truncated GIP varint");
        const uint8_t b=bytes[offset++];
        value |= uint32_t(b & 0x7f) << (7*i);
        if (!(b&0x80)) return value;
    }
    throw std::runtime_error("GIP varint exceeds four bytes");
}
std::vector<GipPacket> parse_gip(std::span<const uint8_t> bytes) {
    std::vector<GipPacket> packets;
    size_t offset=0;
    while (offset<bytes.size()) {
        if (bytes.size()-offset<4) throw std::runtime_error("Truncated GIP header");
        GipPacket pkt{bytes[offset],bytes[offset+1],bytes[offset+2],0,{}};
        offset+=3;
        const uint32_t size=varint(bytes,offset);
        if (pkt.options&0x80) pkt.chunk_offset=varint(bytes,offset);
        if (size>bytes.size()-offset) throw std::runtime_error("GIP payload exceeds received bytes");
        pkt.payload.assign(bytes.begin()+offset,bytes.begin()+offset+size);
        offset+=size;
        packets.push_back(std::move(pkt));
    }
    return packets;
}
std::vector<std::array<uint8_t,16>> identification_interfaces(std::span<const uint8_t> bytes) {
    if(bytes.size()<32)throw std::runtime_error("Truncated identification");
    const auto relative=le16(bytes,28);
    if(!relative)return {};
    const size_t offset=16+size_t(relative);
    if(offset>=bytes.size())throw std::runtime_error("Interfaces outside identification");
    const size_t count=bytes[offset];
    if(count>(bytes.size()-offset-1)/16)throw std::runtime_error("Truncated interface GUIDs");
    std::vector<std::array<uint8_t,16>> result(count);
    for(size_t i=0;i<count;++i)std::copy_n(bytes.begin()+offset+1+16*i,16,result[i].begin());
    return result;
}
GamepadCapabilities identification_capabilities(std::span<const uint8_t> bytes) {
    // GUID byte order follows the GIP little-endian wire representation.
    constexpr std::array<uint8_t,16> share{0xfe,0xd2,0xdd,0xec,0x87,0xd3,0x94,0x42,0xbd,0x96,0x1a,0x71,0x2e,0x3d,0xc7,0x7d};
    constexpr std::array<uint8_t,16> dli{0x6b,0xe5,0xf2,0x87,0xbb,0xc3,0xb1,0x49,0x82,0x65,0xff,0xff,0xf3,0x77,0x99,0xee};
    GamepadCapabilities result;
    for(const auto &guid:identification_interfaces(bytes)){
        result.share|=std::equal(guid.begin(),guid.end(),share.begin());
        result.dli|=std::equal(guid.begin(),guid.end(),dli.begin());
    }
    return result;
}
BatteryStatus parse_battery_status(std::span<const uint8_t> bytes) {
    if(bytes.size()<4)throw std::runtime_error("Truncated GIP status");
    return {(bytes[0]&0x80)!=0,static_cast<uint8_t>((bytes[0]>>2)&3),static_cast<uint8_t>(bytes[0]&3)};
}
AudioFormat audio_format(uint8_t code) {
    switch(code){
    case 5:return {code,1,16000,256};
    case 9:return {code,1,24000,384};
    case 16:return {code,2,48000,1536};
    default:throw std::runtime_error("Unsupported GIP PCM format");
    }
}
std::vector<std::array<uint8_t,2>> identification_audio_formats(std::span<const uint8_t> bytes) {
    if(bytes.size()<32)throw std::runtime_error("Truncated identification");
    const auto relative=le16(bytes,20);if(!relative)return {};
    const size_t offset=16+size_t(relative);
    if(offset>=bytes.size())throw std::runtime_error("Audio formats outside identification");
    const size_t count=bytes[offset];
    if(count>(bytes.size()-offset-1)/2)throw std::runtime_error("Truncated audio formats");
    std::vector<std::array<uint8_t,2>> out;
    for(size_t i=0;i<count;++i)out.push_back({bytes[offset+1+2*i],bytes[offset+2+2*i]});
    return out;
}
GamepadInput parse_gamepad(std::span<const uint8_t> bytes,GamepadCapabilities capabilities) {
    if (bytes.size()<14) throw std::runtime_error("Truncated gamepad input");
    GamepadInput result{le16(bytes,0),le16(bytes,2),le16(bytes,4),0,0,0,0};
    auto signed16=[&](size_t offset) {
        const uint16_t value=le16(bytes,offset);
        int16_t out; std::memcpy(&out,&value,sizeof(out)); return out;
    };
    result.left_x=signed16(6); result.left_y=signed16(8);
    result.right_x=signed16(10); result.right_y=signed16(12);
    if(capabilities.share){
        const size_t tail=18+(capabilities.dli?8:0);
        if(bytes.size()<14+tail)throw std::runtime_error("Truncated Share input");
        result.share=bytes[bytes.size()-tail]!=0;
    }
    return result;
}
RadioEvent parse_radio(std::span<const uint8_t> bytes) {
    if (bytes.size()<8) throw std::runtime_error("Truncated MT76 USB message");
    const uint32_t info=le32(bytes,0);
    const unsigned port=(info>>27)&7, sequence=(info>>16)&15, event=(info>>20)&15;
    if (sequence==1) return {};
    auto body=bytes.subspan(4,bytes.size()-8); // transport header + zero trailer
    RadioEvent result;
    if (port==1 && event==4) { result.kind=RadioEvent::Kind::button; return result; }
    if (port==1 && event==14) {
        if (body.empty()) throw std::runtime_error("Truncated client loss event");
        result.kind=RadioEvent::Kind::lost; result.wcid=body[0]; return result;
    }
    if (port!=0 && !(port==1 && event==12)) return {};
    if (body.size()<32) throw std::runtime_error("Truncated MT76 RXWI");
    const uint32_t rxinfo=le32(body,0),ctl=le32(body,4);
    result.wcid=static_cast<uint8_t>(ctl&255);
    body=body.subspan(32);
    const size_t padding=(rxinfo&(1u<<14)) ? 2 : 0;
    const size_t mpdu=(ctl>>16)&0x3fff;
    // MT76 can loop its own beacon back with a size larger than the USB payload
    // (dlundqvist/xone 83bd053). Drop it before reading any WLAN header.
    if(body.size()<mpdu+padding)return {};
    if(body.size()<24)return {};
    const uint16_t fc=le16(body,0), type=fc&0x00fc;
    size_t header=24;
    if ((fc&0x000c)==0x0008) {
        if ((fc&0x0300)==0x0300) header+=6;
        if (fc&0x0080) header+=2;
        if ((fc&0x0080) && (fc&0x8000)) header+=4;
    }
    if (mpdu<header) throw std::runtime_error("Invalid MPDU length or padding");
    result.address.assign(body.begin()+10,body.begin()+16);
    if(type==0x0000 && ((result.address[0]&1) || std::all_of(result.address.begin(),result.address.end(),[](uint8_t b){return b==0;})))return {};
    const auto payload=body.subspan(header+padding,mpdu-header);
    result.payload.assign(payload.begin(),payload.end());
    if (type==0x0088) result.kind=RadioEvent::Kind::gip;
    else if (type==0x0000) result.kind=RadioEvent::Kind::association;
    else if (type==0x00a0) result.kind=RadioEvent::Kind::disassociation;
    else if (type==0x0070) {
        if (payload.size()<2 || payload[0]!=0x70) throw std::runtime_error("Invalid wireless client command");
        result.kind=RadioEvent::Kind::client_command; result.command=payload[1];
    }
    return result;
}
std::string hex(std::span<const uint8_t> bytes) {
    constexpr char digits[]="0123456789abcdef";
    std::string result; result.reserve(bytes.size()*2);
    for (uint8_t b : bytes) { result+=digits[b>>4]; result+=digits[b&15]; }
    return result;
}
}

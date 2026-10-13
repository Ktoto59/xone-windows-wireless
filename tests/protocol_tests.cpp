// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol.hpp"
extern "C" {
#include "mt76/mt76.h"
}
#include <iostream>
#include <stdexcept>
#include <functional>
#include <array>
#include <map>
#include <cstring>

namespace {
unsigned passed=0;
void check(bool condition,const char *message) { if (!condition) throw std::runtime_error(message); }
void test(const char *name,const std::function<void()> &body) {
    body(); ++passed; std::cout << "PASS " << name << '\n';
}
void rejects(const std::function<void()> &body) {
    bool caught=false;
    try { body(); } catch (const std::runtime_error &) { caught=true; }
    check(caught,"Malformed input was accepted");
}
void put32(std::vector<uint8_t> &out,size_t offset,uint32_t v) {
    for (unsigned i=0;i<4;++i) out.at(offset+i)=static_cast<uint8_t>(v>>(8*i));
}
std::vector<uint8_t> firmware_fixture(uint32_t ilm=4,uint32_t dlm=4) {
    std::vector<uint8_t> data(size_t(32)+ilm+dlm,0x55);
    put32(data,0,ilm); put32(data,4,dlm);
    std::memcpy(data.data()+16,"201703281033____",16);
    return data;
}
std::vector<uint8_t> radio_gip(bool l2pad=false) {
    // RX-FCE + RXWI + 26-byte QoS header + optional padding + GIP + trailer.
    const std::array<uint8_t,18> gip{0x20,0,7,14,0x10,0,0xff,3,0,0,0,0x80,0xff,0x7f,0,0,0xff,0xff};
    std::vector<uint8_t> raw(4+32+26+(l2pad ? 2 : 0)+gip.size()+4);
    put32(raw,4,l2pad ? (1u<<14) : 0);
    put32(raw,8,(uint32_t(26+gip.size())<<16)|2);
    raw[36]=0x88; raw[37]=0x02;
    std::copy(gip.begin(),gip.end(),raw.begin()+62+(l2pad ? 2 : 0));
    return raw;
}
struct Mock {
    std::map<uint32_t,uint32_t> registers;
    std::vector<std::vector<uint8_t>> messages;
    std::vector<uint32_t> dma_offsets;
    std::vector<uint32_t> rf_on_start;
    unsigned controls=0;
    bool fail_read=false,short_read=false,short_bulk=false;
    usb_device usb{};
    device dev{};
    xone_mt76 mt{};
    explicit Mock(const std::vector<uint8_t> &fw) {
        usb={this,control,bulk,0}; dev={fw.data(),fw.size(),false}; mt.dev=&dev; mt.udev=&usb;
    }
    static int control(void *context,u8 req,u8 type,u16 value,u16 index,void *data,u16 length,unsigned) {
        auto &m=*static_cast<Mock *>(context); ++m.controls;
        const uint32_t reg=(uint32_t(value)<<16)|index;
        if (type&0x80) {
            if (m.fail_read) return -EREMOTEIO;
            if (m.short_read) return 2;
            uint32_t result=m.registers[reg];
            if (reg==MT_FCE_DMA_LEN) result |= 0xc0000000;
            if (reg==MT_EFUSE_CTRL) result &= ~MT_EFUSE_CTRL_KICK;
            if (length==4) std::memcpy(data,&result,4);
        } else if (req==MT_VEND_DEV_MODE) {
            m.rf_on_start.push_back(m.registers[0x0130]);
            // Fresh upload writes zero at DMA_ADDR, reset starts with nonzero.
            m.registers[MT_FCE_DMA_ADDR]=m.registers[MT_FCE_DMA_ADDR] ? 0x80000000 : 1;
        } else if (length==4) {
            uint32_t result; std::memcpy(&result,data,4); m.registers[reg]=result;
            if (reg==MT_FCE_DMA_ADDR && (req==MT_VEND_WRITE_CFG)) m.dma_offsets.push_back(result);
        }
        return length;
    }
    static int bulk(void *context,u8 pipe,void *data,int length,int *actual,unsigned) {
        auto &m=*static_cast<Mock *>(context);
        check(pipe==0x04,"Wrong USB output endpoint");
        const auto *bytes=static_cast<uint8_t *>(data);
        m.messages.emplace_back(bytes,bytes+length);
        *actual=m.short_bulk ? length-1 : length;
        return 0;
    }
};
}
int main() {
    static_assert(sizeof(mt76_fw_header)==32);
    static_assert(sizeof(mt76_rxwi)==32);
    static_assert(sizeof(mt76_txwi)==20);
    static_assert(sizeof(ieee80211_hdr_3addr)==24);
    static_assert(offsetof(ieee80211_mgmt,u)==24);
    try {
        test("firmware valid segments",[] { const auto f=xone::validate_firmware(firmware_fixture()); check(f.ilm==4 && f.dlm==4,"Wrong segments"); });
        test("firmware truncation and integer overflow",[] {
            auto f=firmware_fixture(); f.resize(31); rejects([&] { xone::validate_firmware(f); });
            f=firmware_fixture(); put32(f,0,0xfffffff0); put32(f,4,0x10); rejects([&] { xone::validate_firmware(f); });
            f=firmware_fixture(); f.pop_back(); rejects([&] { xone::validate_firmware(f); });
        });
        test("GIP gamepad and signed axes",[] {
            const std::array<uint8_t,18> b{0x20,0,7,14,0x10,0,0xff,3,0,0,0,0x80,0xff,0x7f,0,0,0xff,0xff};
            const auto packets=xone::parse_gip(b); check(packets.size()==1,"Wrong packet count");
            const auto p=xone::parse_gamepad(packets[0].payload);
            check(p.buttons==0x10 && p.left_trigger==1023 && p.left_x==-32768 && p.left_y==32767 && p.right_y==-1,"Wrong input mapping");
        });
        test("GIP variable length with even-header padding",[] {
            std::vector<uint8_t> b{4,0x20,1,0x80,0x81,0}; b.resize(6+128);
            const auto p=xone::parse_gip(b); check(p[0].payload.size()==128,"Wrong padded length");
        });
        test("GIP chunk start and concatenated packets",[] {
            const std::array<uint8_t,12> b{4,0xe0,1,0x82,0,10,9,8,3,0x20,2,0};
            const auto p=xone::parse_gip(b); check(p.size()==2 && p[0].chunk_offset==10 && p[0].payload.size()==2,"Wrong chunk decode");
        });
        test("GIP rejects truncated and excessive varints",[] {
            const std::array<uint8_t,4> a{0x20,0,1,0x80}; rejects([&] { xone::parse_gip(a); });
            const std::array<uint8_t,7> b{0x20,0,1,0x80,0x80,0x80,0x80}; rejects([&] { xone::parse_gip(b); });
            const std::array<uint8_t,4> c{0x20,0,1,14}; rejects([&] { xone::parse_gip(c); });
            const std::array<uint8_t,13> d{}; rejects([&] { xone::parse_gamepad(d); });
        });
        test("MT76 radio frame strips RXWI and L2 padding",[] {
            for (bool padding : {false,true}) {
                const auto r=xone::parse_radio(radio_gip(padding));
                check(r.kind==xone::RadioEvent::Kind::gip && r.wcid==2,"Wrong radio event");
                const auto p=xone::parse_gamepad(xone::parse_gip(r.payload)[0].payload);
                check(p.left_trigger==1023,"Wrong padded payload");
            }
        });
        test("MT76 rejects malformed MPDU",[] {
            auto raw=radio_gip(); put32(raw,8,(uint32_t(1000)<<16)|2);
            check(xone::parse_radio(raw).kind==xone::RadioEvent::Kind::ignored,"Truncated loopback reached frame dispatch");
            raw=radio_gip(); put32(raw,8,(uint32_t(10)<<16)|2); rejects([&] { xone::parse_radio(raw); });
            raw.resize(7); rejects([&] { xone::parse_radio(raw); });
        });
        test("MT76 rejects invalid association addresses and drops short loopback",[] {
            auto raw=radio_gip();raw[36]=0;raw[37]=0;put32(raw,8,24u<<16);
            for(const std::array<uint8_t,6> mac : {std::array<uint8_t,6>{},std::array<uint8_t,6>{255,255,255,255,255,255},std::array<uint8_t,6>{1,2,3,4,5,6}}){
                std::copy(mac.begin(),mac.end(),raw.begin()+46);
                check(xone::parse_radio(raw).kind==xone::RadioEvent::Kind::ignored,"Invalid MAC allocated a client");
            }
            raw[46]=2;check(xone::parse_radio(raw).kind==xone::RadioEvent::Kind::association,"Valid unicast association rejected");
            raw=radio_gip();raw.resize(4+32+12+4);
            check(xone::parse_radio(raw).kind==xone::RadioEvent::Kind::ignored,"Short loopback parsed an absent WLAN header");
        });
        test("MT76 button and client loss events",[] {
            std::vector<uint8_t> raw(8); put32(raw,0,(1u<<27)|(4u<<20));
            check(xone::parse_radio(raw).kind==xone::RadioEvent::Kind::button,"Wrong button event");
            raw.resize(9); put32(raw,0,(1u<<27)|(14u<<20)); raw[4]=3;
            check(xone::parse_radio(raw).wcid==3,"Wrong lost client");
        });
        test("upstream LED command exact USB framing",[] {
            const auto fw=firmware_fixture(); Mock mock(fw);
            check(xone_mt76_set_led_mode(&mock.mt,XONE_MT_LED_OFF)==0,"LED command failed");
            const std::vector<uint8_t> expected{4,0,0,0x51,2,0,0,0,0,0,0,0};
            check(mock.messages.size()==1 && mock.messages[0]==expected,"MT76 LED bytes differ");
        });
        test("upstream firmware DMA streaming and chunk boundaries",[] {
            const auto fw=firmware_fixture(0x3804,4); Mock mock(fw);
            check(xone_mt76_load_firmware(&mock.mt,"mock")==0 && !mock.usb.sticky_error,"Firmware upload failed");
            const std::vector<uint32_t> expected{0x080000,0x083800,0x110800,0};
            check(mock.dma_offsets==expected,"Wrong firmware DMA offsets");
            check(mock.messages.size()==3 && mock.messages[0].size()==0x3808 && mock.messages[1].size()==12,"Wrong firmware chunks");
        });
        test("cold and warm firmware startup clear RF patch before MCU execution",[] {
            constexpr uint32_t patch=0x0130;const auto fw=firmware_fixture();
            for(bool warm : {false,true}){
                Mock mock(fw);mock.registers[patch]=BIT(19)|0x123;
                if(warm)mock.registers[MT_FCE_DMA_ADDR]=1;
                check(xone_mt76_load_firmware(&mock.mt,"mock")==0 && !mock.usb.sticky_error,"Firmware startup failed");
                check(!mock.rf_on_start.empty() && mock.rf_on_start.back()==0x123,"MCU ran before the RF patch or unrelated bits changed");
                if(warm)check(mock.messages.empty(),"Warm reset uploaded firmware again");
            }
        });
        test("upstream full radio init and beacon framing",[] {
            const auto fw=firmware_fixture(); Mock mock(fw);
            check(xone_mt76_init_radio(&mock.mt)==0 && !mock.usb.sticky_error,"Radio init failed");
            check(mock.mt.address[0]==0x62 && mock.mt.address[1]==0x45 && mock.mt.address[2]==0xbd,"Wrong adapter MAC prefix");
            check(mock.mt.channel && mock.mt.channel->index==0xa5,"Wrong final channel");
            check(xone_mt76_set_pairing(&mock.mt,true)==0,"Pair beacon failed");
            const auto &beacon=mock.messages.back();
            check(beacon.size()==88 && beacon[74]==1 && beacon[78]==165,"Wrong pair beacon wire layout/channel");
            check(mock.registers[MT_RX_FILTR_CFG]==0x014f13,"Pairing RX filter blocks discovery");
            check(xone_mt76_set_pairing(&mock.mt,false)==0 && mock.registers[MT_RX_FILTR_CFG]==0x017f17,"Pairing RX filter not restored");
            bool normal_routing=false,wow_disabled=false;
            for(const auto &message:mock.messages)if(message.size()>=9 && message[2]==0x80 && message[3]==0x51){
                if(message[4]==3 && message[5]==0 && message[6]==0 && message[7]==0 && message[8]==0)normal_routing=true;
                if(message.size()>=10 && message[4]==1 && message[5]==0 && message[6]==0 && message[7]==0 && message[8]==0 && message[9]==165)wow_disabled=true;
            }
            check(normal_routing && wow_disabled,"Radio init retained wake-on-wireless traffic routing");
        });
        test("I/O failure is latched and prevents subsequent transfers",[] {
            const auto fw=firmware_fixture(); Mock mock(fw); mock.short_read=true;
            xone_mt76_load_firmware(&mock.mt,"mock");
            check(mock.usb.sticky_error==-EREMOTEIO && mock.controls==1 && mock.messages.empty(),"I/O fault was ignored");
            xone_mt76_set_led_mode(&mock.mt,XONE_MT_LED_ON);
            check(mock.messages.empty(),"Write issued after transport failure");
        });
        test("short bulk write fails instead of success",[] {
            const auto fw=firmware_fixture(); Mock mock(fw); mock.short_bulk=true;
            check(xone_mt76_set_led_mode(&mock.mt,XONE_MT_LED_ON)==-EREMOTEIO,"Partial bulk write accepted");
        });
        std::cout << passed << " tests passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}

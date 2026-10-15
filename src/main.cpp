// SPDX-License-Identifier: GPL-2.0-or-later
#include "winusb_transport.hpp"
#include "protocol.hpp"
#include "session.hpp"
#include "raw_gip_router.hpp"
#include "auto_gip_router.hpp"
#ifdef XONE_NATIVE_USB
#include "native_device_route.hpp"
#endif
#include "runtime_files.hpp"
extern "C" {
#include "mt76/mt76.h"
}
#include <xinput.h>
#include <bcrypt.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>
#include <stdexcept>
#include <algorithm>
#include <deque>
#include <sstream>
#include "crash_trace.hpp"

namespace {
std::atomic_bool interrupted=false;
BOOL WINAPI console_handler(DWORD event) {
    if (event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT) { interrupted=true; return TRUE; }
    return FALSE;
}
struct TickTimer {
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    TickTimer(){if(!timer)throw std::runtime_error("Cannot create radio scheduler timer");}
    ~TickTimer(){CloseHandle(timer);}
    void wait(unsigned milliseconds){
        LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(milliseconds)*10000;
        if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE) || WaitForSingleObject(timer,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("Radio scheduler timer failed");
    }
};
void help() {
    std::cout << "xone Windows transport prototype (not a complete controller driver)\n"
        "  xone-probe list\n"
        "  xone-probe xinput [--seconds 20]\n"
        "  xone-probe firmware-info --firmware FILE\n"
        "  xone-probe probe [--instance USB-instance-id]\n"
        "  xone-probe radio-test --firmware FILE [--seconds 30] [--capture FILE.jsonl]\n"
        "                       [--pair-beacon] [--verbose] [--instance USB-instance-id]\n"
        "  xone-probe controller-test --firmware FILE [--seconds 120] [--capture FILE.jsonl]\n"
        "                            [--virtual-xinput]\n"
        "  xone-probe virtual-test (ViGEmBus/XInput smoke test; no USB access)\n"
        "  xone-probe run --firmware FILE [--stop-file FILE] [--seconds 0] [--verbose]\n"
        "                 [--output vigem|native-gip|usb-gip] [--status-file FILE.json]\n"
        "                 [--usb-gip-mac HEX12 --usb-gip-port 33241]\n"
        "                 [--usb-gip-fallback none|native-gip|vigem]\n"
        "                 [--usb-gip-peer HEX12:PORT] (additional USB radio peer)\n"
        "  xone-probe xinput-rumble --slot 0 [--large 16000] [--small 0] [--seconds 1]\n"
        "  xone-probe decode HEX-USB-PACKET\n"
        "  xone-probe decode-gip HEX-GIP-PACKET\n"
        "Raw USB commands need WinUSB binding. list/xinput/firmware-info/decode do not.\n"
        "radio-test does not authenticate controllers or publish input/audio devices.\n";
}
struct Options {
    std::wstring firmware, instance, capture, stop_file,status_file,output=L"vigem";
    std::wstring usb_gip_mac,usb_gip_fallback=L"none";
    uint16_t usb_gip_port=33241;
    unsigned usb_auto_slots=0;
    std::vector<std::pair<std::wstring,uint16_t>> usb_gip_peers;
    unsigned seconds=30;
    unsigned slot=0,large=16000,small_motor=0;
    bool verbose=false,pair=false,virtual_xinput=false,idle_channel_scan=false,native_usb=false;
};
Options options(int argc,wchar_t **argv) {
    Options result;
    if(std::wstring(argv[1])==L"run"){result.seconds=0;result.virtual_xinput=true;}
    if(std::wstring(argv[1])==L"xinput-rumble")result.seconds=1;
    for (int i=2;i<argc;++i) {
        const std::wstring key=argv[i];
        if (key==L"--verbose") result.verbose=true;
        else if (key==L"--pair-beacon") result.pair=true;
        else if (key==L"--idle-channel-scan") result.idle_channel_scan=true;
        else if (key==L"--native-usb") result.native_usb=true;
        else if (key==L"--virtual-xinput") result.virtual_xinput=true;
        else {
            if (i+1>=argc) throw std::runtime_error("Missing value for " + xone::utf8(key));
            const std::wstring value=argv[++i];
            if (key==L"--firmware") result.firmware=value;
            else if(key==L"--output") {if(value!=L"vigem" && value!=L"native-gip" && value!=L"usb-gip")throw std::runtime_error("Output must be vigem, native-gip or usb-gip");result.output=value;result.virtual_xinput=value!=L"usb-gip";}
            else if(key==L"--usb-gip-mac"){
                if(value.size()!=12 || !std::all_of(value.begin(),value.end(),[](wchar_t c){return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f') || (c>=L'A' && c<=L'F');}))throw std::runtime_error("USB GIP MAC must be twelve hex digits");
                result.usb_gip_mac=value;
            }else if(key==L"--usb-gip-port"){
                size_t consumed=0;const auto port=std::stoul(value,&consumed);
                if(consumed!=value.size() || port<1024 || port>65535)throw std::runtime_error("Invalid local USB GIP port");result.usb_gip_port=static_cast<uint16_t>(port);
            }else if(key==L"--usb-auto-slots"){
                size_t consumed=0;const auto count=std::stoul(value,&consumed);
                if(consumed!=value.size() || !count || count>16)throw std::runtime_error("Automatic USB slots must be 1..16");
                result.usb_auto_slots=static_cast<unsigned>(count);
            }else if(key==L"--usb-gip-fallback"){
                if(value!=L"none" && value!=L"native-gip" && value!=L"vigem")throw std::runtime_error("USB GIP fallback must be none, native-gip or vigem");
                result.usb_gip_fallback=value;
            }else if(key==L"--usb-gip-peer"){
                if(result.usb_gip_peers.size()>=15 || value.size()<17 || value[12]!=L':' || !std::all_of(value.begin(),value.begin()+12,[](wchar_t c){return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f') || (c>=L'A' && c<=L'F');}))throw std::runtime_error("USB GIP peer must be HEX12:PORT");
                size_t consumed=0;const auto port=std::stoul(value.substr(13),&consumed);
                if(consumed!=value.size()-13 || port<1024 || port>65535)throw std::runtime_error("Invalid additional USB GIP port");
                result.usb_gip_peers.emplace_back(value.substr(0,12),static_cast<uint16_t>(port));
            }
            else if (key==L"--instance") result.instance=value;
            else if (key==L"--capture") result.capture=value;
            else if (key==L"--stop-file") result.stop_file=value;
            else if (key==L"--status-file") result.status_file=value;
            else if(key==L"--slot" || key==L"--large" || key==L"--small") {
                size_t consumed=0;const auto n=std::stoul(value,&consumed);
                if(consumed!=value.size() || n>(key==L"--slot"?3u:65535u))throw std::runtime_error("Invalid XInput rumble value");
                if(key==L"--slot")result.slot=static_cast<unsigned>(n);
                else if(key==L"--large")result.large=static_cast<unsigned>(n);
                else result.small_motor=static_cast<unsigned>(n);
            }
            else if (key==L"--seconds") {
                size_t consumed=0;
                const auto n=std::stoul(value,&consumed);
                if (consumed!=value.size() || n>86400 || (!n && std::wstring(argv[1])!=L"run")) throw std::runtime_error("seconds must be 1..86400 (0 allowed for run)");
                result.seconds=static_cast<unsigned>(n);
            } else throw std::runtime_error("Unknown option: " + xone::utf8(key));
        }
    }
    if(result.usb_gip_fallback!=L"none" && result.output!=L"usb-gip")throw std::runtime_error("USB GIP fallback requires --output usb-gip");
    if(result.usb_auto_slots && (result.output!=L"usb-gip" || !result.usb_gip_mac.empty() || !result.usb_gip_peers.empty() || result.usb_gip_fallback!=L"none" || unsigned(result.usb_gip_port)+2*result.usb_auto_slots>65535))throw std::runtime_error("Automatic USB requires an exclusive slot pool, no fixed MACs or fallback");
    if(!result.usb_gip_peers.empty()){
        if(result.output!=L"usb-gip" || result.usb_gip_mac.empty())throw std::runtime_error("Additional USB peers require --output usb-gip and --usb-gip-mac");
        auto peers=result.usb_gip_peers;peers.emplace_back(result.usb_gip_mac,result.usb_gip_port);
        for(size_t a=0;a<peers.size();++a)for(size_t b=a+1;b<peers.size();++b)
            if(!_wcsicmp(peers[a].first.c_str(),peers[b].first.c_str()) || peers[a].second==peers[b].second)throw std::runtime_error("USB GIP peers need unique MACs and ports");
    }
    return result;
}
std::vector<uint8_t> file_bytes(const std::wstring &path) {
    if (path.empty()) throw std::runtime_error("Specify --firmware FILE");
    std::ifstream input(std::filesystem::path(path),std::ios::binary|std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read firmware file");
    const auto size=input.tellg();
    if (size<0 || size>4*1024*1024) throw std::runtime_error("Firmware file exceeds 4 MiB");
    std::vector<uint8_t> data(static_cast<size_t>(size));
    input.seekg(0);
    if (!data.empty() && !input.read(reinterpret_cast<char *>(data.data()),static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read complete firmware file");
    return data;
}
std::string sha256(std::span<const uint8_t> bytes) {
    std::array<uint8_t,32> hash{};
    const auto status=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
        const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),hash.data(),static_cast<ULONG>(hash.size()));
    if (status<0) throw std::runtime_error("BCryptHash failed");
    return xone::hex(hash);
}
void firmware_info(std::span<const uint8_t> bytes) {
    const auto info=xone::validate_firmware(bytes);
    std::cout << "MT76 firmware: ILM=" << info.ilm << " DLM=" << info.dlm
        << " build=" << info.build << " version=" << info.version << " timestamp=" << info.timestamp
        << "\nSHA256=" << sha256(bytes) << '\n';
}
xone::DeviceInfo select_device(const Options &opt) {
    auto list=xone::adapters();
    if (list.empty()) throw std::runtime_error("No supported Xbox Wireless adapter is present");
    if (!opt.instance.empty()) {
        for (auto &d : list) if (!_wcsicmp(d.instance.c_str(),opt.instance.c_str())) return d;
        throw std::runtime_error("Requested adapter instance was not found");
    }
    if (list.size()!=1) throw std::runtime_error("More than one adapter: specify --instance from list output");
    return list.front();
}
void show_packet(const xone::GipPacket &pkt) {
    std::cout << "GIP cmd=0x" << std::hex << unsigned(pkt.command) << " options=0x" << unsigned(pkt.options)
        << std::dec << " sequence=" << unsigned(pkt.sequence) << " length=" << pkt.payload.size();
    if (pkt.options&0x80) std::cout << " chunk-offset=" << pkt.chunk_offset;
    std::cout << '\n';
    if (pkt.command==0x20 && !(pkt.options&0xa0)) {
        const auto pad=xone::parse_gamepad(pkt.payload);
        std::cout << "  buttons=0x" << std::hex << pad.buttons << std::dec << " LT=" << pad.left_trigger
            << " RT=" << pad.right_trigger << " LX=" << pad.left_x << " LY=" << pad.left_y
            << " RX=" << pad.right_x << " RY=" << pad.right_y << '\n';
    }
}
void show_radio(const xone::RadioEvent &event) {
    if (event.kind==xone::RadioEvent::Kind::ignored) return;
    std::cout << "Radio event=" << static_cast<int>(event.kind) << " WCID=" << unsigned(event.wcid);
    if (!event.address.empty()) std::cout << " MAC=" << xone::hex(event.address);
    if (event.kind==xone::RadioEvent::Kind::client_command) std::cout << " command=" << unsigned(event.command);
    std::cout << '\n';
    if (event.kind==xone::RadioEvent::Kind::gip) for (const auto &pkt : xone::parse_gip(event.payload)) show_packet(pkt);
}
std::vector<uint8_t> unhex(const std::wstring &input) {
    if (input.size()%2) throw std::runtime_error("Hex packet needs an even number of digits");
    auto digit=[](wchar_t ch) -> unsigned {
        if (ch>=L'0' && ch<=L'9') return unsigned(ch-L'0');
        if (ch>=L'a' && ch<=L'f') return unsigned(ch-L'a'+10);
        if (ch>=L'A' && ch<=L'F') return unsigned(ch-L'A'+10);
        throw std::runtime_error("Invalid hex digit");
    };
    std::vector<uint8_t> result;
    for (size_t i=0;i<input.size();i+=2) result.push_back(static_cast<uint8_t>((digit(input[i])<<4)|digit(input[i+1])));
    return result;
}
int xinput_baseline(unsigned seconds) {
    std::array<XINPUT_STATE,4> previous{};
    std::array<bool,4> connected{};
    bool seen=false;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while (!interrupted && std::chrono::steady_clock::now()<end) {
        for (DWORD i=0;i<4;++i) {
            XINPUT_STATE state{};
            const DWORD status=XInputGetState(i,&state);
            if (status!=ERROR_SUCCESS && status!=ERROR_DEVICE_NOT_CONNECTED)
                throw std::runtime_error("XInputGetState: " + xone::win_error(status));
            const bool present=status==ERROR_SUCCESS;
            if (present && (!connected[i] || state.dwPacketNumber!=previous[i].dwPacketNumber)) {
                const auto &p=state.Gamepad;
                std::cout << "XInput[" << i << "] packet=" << state.dwPacketNumber
                    << " buttons=0x" << std::hex << p.wButtons << std::dec << " LT=" << unsigned(p.bLeftTrigger)
                    << " RT=" << unsigned(p.bRightTrigger) << " LX=" << p.sThumbLX << " LY=" << p.sThumbLY
                    << " RX=" << p.sThumbRX << " RY=" << p.sThumbRY << '\n';
                seen=true;
            } else if (!present && connected[i]) std::cout << "XInput[" << i << "] disconnected\n";
            connected[i]=present; previous[i]=state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!seen) std::cout << "No XInput controller observed.\n";
    return seen ? 0 : 3;
}
int virtual_test() {
    for(unsigned cycle=0;cycle<3;++cycle) {
        xone::VigemOutput output;output.connect(1);DWORD slot=output.slot(1);
        const xone::GamepadInput pad{16,1023,512,-32768,32767,1234,-5678};output.update(1,pad,false);
        const auto expected=xone::virtual_report(pad);XINPUT_STATE observed{};bool matched=false;
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<end) {
            output.feedback();
            for(DWORD candidate=0;candidate<4;++candidate)if(XInputGetState(candidate,&observed)==ERROR_SUCCESS) {
                const auto &p=observed.Gamepad;
                if(p.wButtons==expected.buttons && p.bLeftTrigger==expected.left_trigger && p.bRightTrigger==expected.right_trigger && p.sThumbLX==expected.left_x && p.sThumbLY==expected.left_y && p.sThumbRX==expected.right_x && p.sThumbRY==expected.right_y){slot=candidate;matched=true;break;}
            }
            if(matched)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(!matched) {
            const auto &p=observed.Gamepad;
            std::cerr<<"Observed virtual slot="<<slot<<" buttons="<<p.wButtons<<" LT="<<unsigned(p.bLeftTrigger)<<" RT="<<unsigned(p.bRightTrigger)<<" LX="<<p.sThumbLX<<" LY="<<p.sThumbLY<<" RX="<<p.sThumbRX<<" RY="<<p.sThumbRY<<"\n";
            throw std::runtime_error("Virtual XInput state did not match the injected report");
        }
        XINPUT_VIBRATION vibration{0x6600,0x9900};
        if(XInputSetState(slot,&vibration)!=ERROR_SUCCESS)throw std::runtime_error("Virtual XInputSetState failed");
        bool feedback=false;const auto rumble_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<rumble_end) {
            const auto f=output.feedback();if(f && f->large==0x66 && f->small_motor==0x99){feedback=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        vibration={0,0};XInputSetState(slot,&vibration);
        if(!feedback)throw std::runtime_error("Virtual rumble feedback did not arrive");
        output.disconnect(1);
        bool removed=false;const auto remove_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<remove_end) {if(XInputGetState(slot,&observed)==ERROR_DEVICE_NOT_CONNECTED){removed=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        if(!removed)throw std::runtime_error("Virtual controller remained in XInput after removal");
        std::cout<<"PASS virtual cycle "<<cycle+1<<": input, rumble feedback, cancellation, removal\n";
    }
    return 0;
}
int xinput_rumble(const Options &opt) {
    if(opt.seconds>5)throw std::runtime_error("Rumble test duration must be 1..5 seconds");
    struct Stop {DWORD slot;~Stop(){XINPUT_VIBRATION zero{};XInputSetState(slot,&zero);}} stop{opt.slot};
    XINPUT_VIBRATION vibration{static_cast<WORD>(opt.large),static_cast<WORD>(opt.small_motor)};
    const DWORD error=XInputSetState(opt.slot,&vibration);if(error!=ERROR_SUCCESS)throw std::runtime_error("XInputSetState: "+xone::win_error(error));
    std::cout<<"XInput rumble slot="<<opt.slot<<" large="<<opt.large<<" small="<<opt.small_motor<<"\n";
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(opt.seconds);
    while(!interrupted && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return 0;
}
class Receiver {
    xone::WinUsbTransport &usb_;
    std::mutex mutex_;
    std::ofstream output_;
    std::vector<std::jthread> threads_;
    std::string error_;
    std::deque<xone::RadioEvent> events_;
    bool session_;
    std::atomic_uint64_t count_=0;
    std::chrono::steady_clock::time_point start_=std::chrono::steady_clock::now();
public:
    Receiver(xone::WinUsbTransport &usb,const std::wstring &path,bool session=false):usb_(usb),session_(session) {
        if (!path.empty()) {
            if (std::filesystem::exists(path)) throw std::runtime_error("Capture file already exists; choose a new filename");
            output_.open(std::filesystem::path(path),std::ios::binary);
            if (!output_) throw std::runtime_error("Cannot create capture file");
        }
        for (const uint8_t pipe : {uint8_t(0x84),uint8_t(0x85)}) {
            threads_.emplace_back([this,pipe](std::stop_token token) {
                try {
                    while (!token.stop_requested() && !interrupted) {
                        auto bytes=usb_.read(pipe);
                        if (bytes.empty()) continue;
                        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start_).count();
                        std::lock_guard lock(mutex_);
                        ++count_;
                        if (output_.is_open()) {
                            output_ << "{\"ms\":" << ms << ",\"pipe\":" << unsigned(pipe) << ",\"hex\":\"" << xone::hex(bytes) << "\"}\n";
                            output_.flush();
                            if (!output_) throw std::runtime_error("Cannot write capture file");
                        }
                        try {
                            auto event=xone::parse_radio(bytes);
                            if (session_) {
                                if (events_.size()>=4096) {
                                    error_="Radio event queue overflow";
                                    throw std::runtime_error(error_);
                                }
                                if (event.kind!=xone::RadioEvent::Kind::ignored) events_.push_back(std::move(event));
                            } else show_radio(event);
                        }
                        catch (const std::exception &e) { std::cerr << "Packet decode: " << e.what() << '\n'; }
                    }
                } catch (const std::exception &e) {
                    std::lock_guard lock(mutex_); if (error_.empty()) error_=e.what();
                }
            });
        }
    }
    ~Receiver() { stop(); }
    void stop() { for (auto &t : threads_) t.request_stop(); threads_.clear(); }
    void check() { std::lock_guard lock(mutex_); if (!error_.empty()) throw std::runtime_error(error_); }
    uint64_t count() const { return count_.load(); }
    std::deque<xone::RadioEvent> drain() { std::lock_guard lock(mutex_); std::deque<xone::RadioEvent> out; out.swap(events_); return out; }
};
void check_mt76(int result,usb_device &usb,const char *operation) {
    if (result || usb.sticky_error) throw std::runtime_error(std::string(operation)+" failed: " + std::to_string(usb.sticky_error ? usb.sticky_error : result));
}
int radio_test(const Options &opt,bool controller=false,bool run=false) {
    std::unique_ptr<xone::RawGipOutput> raw_output;
    if(opt.output==L"usb-gip"){
        if(!run || (!opt.usb_auto_slots && opt.usb_gip_mac.empty()) || opt.virtual_xinput)throw std::runtime_error("USB GIP requires run, a fixed MAC or automatic slots, and no virtual XInput flag");
        if(opt.usb_auto_slots){
#ifdef XONE_NATIVE_USB
            if(opt.native_usb){
                raw_output=std::make_unique<xone::AutoGipRouter>(opt.usb_auto_slots,[&](size_t slot,const xone::Bytes &mac){
                    const auto path=std::filesystem::path(opt.status_file).parent_path()/(L"native-slot-"+std::to_wstring(slot)+L".json");
                    return std::make_unique<xone::NativeDeviceRoute>(static_cast<uint16_t>(opt.usb_gip_port+2*slot),mac,path.wstring(),opt.verbose);
                });
            }else
#else
            if(opt.native_usb)throw std::runtime_error("Use the C++ USB executable for --native-usb");
#endif
            raw_output=std::make_unique<xone::AutoGipRouter>(opt.usb_auto_slots,[&](size_t slot,const xone::Bytes &mac){return std::make_unique<xone::UsbGipBroker>(static_cast<uint16_t>(opt.usb_gip_port+2*slot),mac);});
        }else{
        std::vector<std::unique_ptr<xone::RawGipOutput>> brokers;
        brokers.push_back(std::make_unique<xone::UsbGipBroker>(opt.usb_gip_port,unhex(opt.usb_gip_mac)));
        for(const auto &[mac,port]:opt.usb_gip_peers)brokers.push_back(std::make_unique<xone::UsbGipBroker>(port,unhex(mac)));
        raw_output=std::make_unique<xone::RawGipRouter>(std::move(brokers));
        }
    }
    std::unique_ptr<xone::InputOutput> output;
    const bool usb_fallback=opt.output==L"usb-gip" && opt.usb_gip_fallback!=L"none";
    const bool native_output=opt.output==L"native-gip" || (usb_fallback && opt.usb_gip_fallback==L"native-gip");
    if(opt.virtual_xinput || usb_fallback) {
        if(!controller)throw std::runtime_error("Virtual output requires controller-test or run");
        if(native_output)output=std::make_unique<xone::NativeGipOutput>();
        else output=std::make_unique<xone::VigemOutput>();
    }
    if(raw_output)std::cout<<"USB GIP target="<<xone::utf8(opt.usb_gip_mac)<<" other_devices_output="<<xone::utf8(opt.usb_gip_fallback)<<'\n';
    auto firmware=file_bytes(opt.firmware);
    firmware_info(firmware);
    if (sha256(firmware)!="48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66")
        throw std::runtime_error("Firmware differs from the xone-supported image; hardware test refused");
    auto adapter=select_device(opt);
    xone::WinUsbTransport usb(adapter);
    usb.describe();
    Receiver receiver(usb,opt.capture,controller);
    device dev{firmware.data(),firmware.size(),opt.verbose};
    xone_mt76 mt{}; mt.dev=&dev; mt.udev=&usb.bridge;
    check_mt76(xone_mt76_load_firmware(&mt,"FW_ACC_00U.bin"),usb.bridge,"Firmware load/reset");
    receiver.check();
    check_mt76(xone_mt76_init_radio(&mt),usb.bridge,"Radio initialization");
    std::cout << "Radio initialized. Host MAC=" << xone::hex(mt.address)
        << " channel=" << unsigned(mt.channel->index) << "\n";
    if (controller) {
        TickTimer tick_timer;
        std::unique_ptr<xone::AudioOutput> audio_output;
        if(run && native_output)audio_output=std::make_unique<xone::NativeAudioOutput>();
        xone::Session session(mt,output.get(),!run || opt.verbose,audio_output.get(),raw_output.get());
        if(opt.idle_channel_scan)session.enable_idle_channel_scan();
        if(opt.pair)session.begin_pairing();
        xone::RuntimeFiles runtime_files([&](const std::string &text){
            const auto destination=std::filesystem::path(opt.status_file),temporary=std::filesystem::path(opt.status_file+L".tmp");
            {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);if(!file || !file.write(text.data(),static_cast<std::streamsize>(text.size())))throw std::runtime_error("Cannot write controller status");}
            if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("Cannot replace controller status file");
        },[&]{return !opt.stop_file.empty() && std::filesystem::exists(opt.stop_file);});
        std::string previous_status;
        auto status_last=std::chrono::steady_clock::time_point{};
        auto save_status=[&](bool running){
            if(opt.status_file.empty())return;
            const auto now=std::chrono::steady_clock::now();
            if(running && now-status_last<std::chrono::milliseconds(100))return;
            status_last=now;
            std::ostringstream json;
            json<<std::boolalpha<<"{\"Running\":"<<running<<",\"Backend\":\""<<xone::utf8(opt.output)<<"\",\"FallbackOutput\":\""<<xone::utf8(opt.usb_gip_fallback)<<"\",\"Pairing\":"<<session.pairing_active()<<",\"RadioChannel\":"<<unsigned(mt.channel->index)<<",\"AutomaticSlots\":"<<opt.usb_auto_slots<<",\"ShareForwardedToWindows\":false,\"BatteryForwardedToWindows\":false,\"Controllers\":[";
            bool first=true;
            for(const auto &c:session.status()){
                if(!first)json<<',';first=false;
                json<<"{\"Wcid\":"<<unsigned(c.wcid)<<",\"Mac\":\""<<c.mac<<"\",\"Kind\":\""<<c.kind<<"\",\"Encrypted\":"<<c.encrypted
                    <<",\"Authentication\":\""<<c.authentication<<"\",\"AuthVersion\":"<<unsigned(c.auth_version)<<",\"PeerAuthVersion\":"<<unsigned(c.peer_auth_version)
                    <<",\"SupportsShare\":"<<c.capabilities.share<<",\"SupportsDli\":"<<c.capabilities.dli<<",\"SharePressed\":"<<c.share_pressed<<",\"Battery\":";
                if(c.battery){
                    constexpr const char *types[]={"none","standard","play-and-charge","unknown"};
                    constexpr const char *levels[]={"low","normal","high","full"};
                    json<<"{\"Type\":\""<<types[c.battery->type]<<"\",\"Level\":\""<<(c.battery->type?levels[c.battery->level]:"unknown")<<"\",\"RawType\":"<<unsigned(c.battery->type)<<",\"RawLevel\":"<<unsigned(c.battery->level)<<'}';
                }else json<<"null";
                json<<",\"MalformedGipPackets\":"<<c.malformed_gip_packets;
                json<<",\"UsbTx\":{\"Packets\":"<<c.usb_tx.packets<<",\"Over8ms\":"<<c.usb_tx.over_8ms
                    <<",\"MaxUs\":"<<c.usb_tx.max_us<<",\"LastUs\":"<<c.usb_tx.last_us<<"},\"Audio\":";
                if(c.audio){json<<"{\"Stage\":\""<<c.audio->stage<<"\",\"InputRate\":"<<c.audio->input_rate<<",\"OutputRate\":"<<c.audio->output_rate<<",\"InputChannels\":"<<unsigned(c.audio->input_channels)<<",\"OutputChannels\":"<<unsigned(c.audio->output_channels)<<",\"ReceivedPackets\":"<<c.audio->received_packets<<",\"SilencePackets\":"<<c.audio->silence_packets<<",\"PlaybackPackets\":"<<c.audio->playback_packets<<",\"CapturePackets\":"<<c.audio->capture_packets<<",\"WindowsEndpoint\":"<<(!c.audio->endpoints.render.empty() && !c.audio->endpoints.capture.empty())<<",\"SyntheticGamepadParent\":"<<c.audio->synthetic_parent<<",\"RenderId\":\""<<c.audio->endpoints.render<<"\",\"CaptureId\":\""<<c.audio->endpoints.capture<<"\"}";}
                else json<<"null";
                json<<'}';
            }
            json<<"],\"RecentRadioRemovals\":[";bool first_removal=true;
            for(const auto &r:session.removals()){if(!first_removal)json<<',';first_removal=false;json<<"{\"Mac\":\""<<r.mac<<"\",\"Reason\":\""<<r.reason<<"\",\"Wcid\":"<<unsigned(r.wcid)<<",\"TickMs\":"<<r.tick_ms<<'}';}
            json<<"],\"RuntimeFileIoErrors\":"<<runtime_files.io_errors()<<"}\n";const auto text=json.str();if(text==previous_status)return;
            runtime_files.publish(text);
            previous_status=text;
        };
        std::cout << "CONTROLLER READY: power on one controller; pair buttons only if it does not reconnect.\n" << std::flush;
        const auto end=opt.seconds?std::chrono::steady_clock::now()+std::chrono::seconds(opt.seconds):std::chrono::steady_clock::time_point::max();
        try {
            while (!interrupted && std::chrono::steady_clock::now()<end && !runtime_files.stop_requested()) {
                receiver.check(); for (const auto &event:receiver.drain()) session.process(event); session.tick();save_status(true);
                std::cout.flush();tick_timer.wait(session.audio_active() || opt.output==L"usb-gip"?1:5);
            }
            session.close();save_status(false);
        } catch (...) {
            try { session.close(); } catch (...) {}
            // A latched USB error can also prevent cleanup; still mark the snapshot stopped.
            try { save_status(false); } catch (...) {}
            throw;
        }
        receiver.stop(); receiver.check();
        std::cout << "Controller session: authenticated=" << session.authenticated() << " input_reports=" << session.inputs() << " rumbles=" << session.rumbles() << " USB_packets=" << receiver.count() << "\n";
        return run || (session.inputs() && session.authenticated()) ? 0 : 3;
    }
    try {
        check_mt76(xone_mt76_set_led_mode(&mt,XONE_MT_LED_BLINK),usb.bridge,"LED command");
        if (opt.pair) check_mt76(xone_mt76_set_pairing(&mt,true),usb.bridge,"Pair beacon");
        const auto start=std::chrono::steady_clock::now(), end=start+std::chrono::seconds(opt.seconds);
        bool beacon=opt.pair;
        while (!interrupted && std::chrono::steady_clock::now()<end) {
            receiver.check();
            if (beacon && std::chrono::steady_clock::now()-start>=std::chrono::seconds(30)) {
                check_mt76(xone_mt76_set_pairing(&mt,false),usb.bridge,"Pair beacon timeout"); beacon=false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (...) {
        xone_mt76_set_pairing(&mt,false); xone_mt76_set_led_mode(&mt,XONE_MT_LED_OFF); throw;
    }
    check_mt76(xone_mt76_set_pairing(&mt,false),usb.bridge,"Pair beacon cleanup");
    check_mt76(xone_mt76_set_led_mode(&mt,XONE_MT_LED_OFF),usb.bridge,"LED cleanup");
    receiver.stop(); receiver.check();
    std::cout << "Received " << receiver.count() << " USB packets. This test does not publish controller/audio devices.\n";
    return 0;
}
}
int wmain(int argc,wchar_t **argv) {
    std::cout.setf(std::ios::unitbuf);
    SetUnhandledExceptionFilter(xone_crash_trace);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(console_handler,TRUE);
    try {
        if (argc<2 || std::wstring(argv[1])==L"--help") { help(); return 0; }
        const std::wstring command=argv[1];
        if (command==L"decode" || command==L"decode-gip") {
            if (argc!=3) throw std::runtime_error("Specify exactly one hex packet");
            const auto bytes=unhex(argv[2]);
            if (command==L"decode") show_radio(xone::parse_radio(bytes));
            else for (const auto &pkt : xone::parse_gip(bytes)) show_packet(pkt);
            return 0;
        }
        const auto opt=options(argc,argv);
        if (command==L"list") {
            const auto list=xone::adapters();
            for (const auto &d : list) std::cout << xone::utf8(d.name) << "\n  instance=" << xone::utf8(d.instance)
                << "\n  service=" << xone::utf8(d.service) << " INF=" << xone::utf8(d.inf)
                << " version=" << xone::utf8(d.version) << "\n  interfaces=" << d.paths.size() << '\n';
            return list.empty() ? 3 : 0;
        }
        if (command==L"xinput") return xinput_baseline(opt.seconds);
        if (command==L"virtual-test") return virtual_test();
        if (command==L"xinput-rumble") return xinput_rumble(opt);
        if (command==L"firmware-info") { const auto bytes=file_bytes(opt.firmware); firmware_info(bytes); return 0; }
        if (command==L"probe") { xone::WinUsbTransport usb(select_device(opt)); usb.describe(); return 0; }
        if (command==L"radio-test") return radio_test(opt);
        if (command==L"controller-test") return radio_test(opt,true);
        if (command==L"run") return radio_test(opt,true,true);
        throw std::runtime_error("Unknown command; use --help");
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n'; return 2;
    }
}

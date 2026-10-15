// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw_gip.hpp"
namespace xone {
// Direct in-process RF <-> USB/IP route, one USB/audio thread per device.
class NativeUsbOutput final:public RawGipOutput {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    NativeUsbOutput(uint16_t port,Bytes mac,uint16_t pid,bool audio,std::wstring status,bool automatic=false,bool diagnostics=false);
    ~NativeUsbOutput()override;
    bool target(std::span<const uint8_t> mac)const override;
    bool automatic()const;
    bool audio_active()const;
    bool claim(std::span<const uint8_t> mac,uint16_t vendor,uint16_t product);
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})override;
    std::vector<RawGipTransmit> poll()override;
    void complete(uint8_t wcid,uint32_t token,int status)override;
};
}

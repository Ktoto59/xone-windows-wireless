// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw_gip.hpp"
namespace xone {
// Direct in-process RF <-> USB/IP route, one USB/audio thread per device.
class NativeUsbOutput final:public RawGipOutput {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    NativeUsbOutput(uint16_t port,Bytes mac,uint16_t pid,bool audio,std::wstring status);
    ~NativeUsbOutput()override;
    bool target(std::span<const uint8_t> mac)const override;
    void event(uint8_t kind,uint8_t wcid,std::span<const uint8_t> data={})override;
    std::vector<RawGipTransmit> poll()override;
    void complete(uint8_t wcid,uint32_t token,int status)override;
};
}

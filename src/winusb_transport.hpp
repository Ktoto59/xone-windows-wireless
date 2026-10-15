// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winusb.h>
#include <string>
#include <vector>
#include <cstdint>
#include <span>
#include "platform/xone_compat.h"
#include "pipe_timeouts.hpp"

namespace xone {
inline constexpr GUID interface_guid = {0x215e9d83,0x9720,0x4a4c,{0x8c,0xf2,0xe8,0xf4,0x0e,0x36,0x6c,0x21}};
struct DeviceInfo {
    std::wstring instance, name, service, inf, version;
    std::vector<std::wstring> paths;
};
std::string utf8(const std::wstring &text);
std::vector<DeviceInfo> adapters();
std::string win_error(DWORD error);
class WinUsbTransport {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb_ = nullptr;
    PipeTimeouts timeouts_;
    bool set_timeout(uint8_t pipe,unsigned timeout);
    static int control_callback(void *, u8, u8, u16, u16, void *, u16, unsigned);
    static int bulk_callback(void *, u8, void *, int, int *, unsigned);
public:
    usb_device bridge{};
    explicit WinUsbTransport(const DeviceInfo &device);
    ~WinUsbTransport();
    WinUsbTransport(const WinUsbTransport &) = delete;
    WinUsbTransport &operator=(const WinUsbTransport &) = delete;
    std::vector<uint8_t> read(uint8_t pipe);
    void describe() const;
};
}

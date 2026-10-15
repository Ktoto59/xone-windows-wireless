// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace xone {
// Owned by one WinUSB interface lifetime. Runtime policy writes are serialized
// by the radio thread; receive threads never modify timeout policies.
class PipeTimeouts {
    std::array<std::optional<unsigned>,256> values_{};
public:
    template<class Apply> bool set(uint8_t pipe,unsigned timeout,Apply &&apply){
        auto &value=values_[pipe];
        if(value && *value==timeout)return true;
        if(!apply(pipe,timeout)){value.reset();return false;}
        value=timeout;return true;
    }
};
}

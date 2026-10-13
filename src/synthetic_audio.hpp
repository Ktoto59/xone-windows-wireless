// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <span>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
namespace xone {
// Windows build 26300 consumes 2ms mono/24k blocks from synthetic audio input.
// SendAudio declares the supplied length in GIP but also prepends two bytes.
// Padding the argument by two bytes preserves all 96 PCM bytes in that length.
inline std::array<std::array<uint8_t,98>,4> synthetic_microphone_blocks(std::span<const uint8_t> pcm){
    if(pcm.size()!=384)throw std::runtime_error("Expected 8ms mono 24k microphone PCM");
    std::array<std::array<uint8_t,98>,4> blocks{};
    for(size_t n=0;n<blocks.size();++n)std::copy_n(pcm.begin()+n*96,96,blocks[n].begin());
    return blocks;
}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "protocol.hpp"
#include <optional>
#include <memory>
#include <array>
namespace xone {
struct VirtualReport { uint16_t buttons; uint8_t left_trigger,right_trigger; int16_t left_x,left_y,right_x,right_y; };
VirtualReport virtual_report(const GamepadInput &input,bool guide=false);
std::array<uint8_t,9> rumble_packet(uint8_t large,uint8_t small_motor);
std::array<uint8_t,14> native_input_report(const GamepadInput &input);
std::optional<std::array<uint8_t,9>> native_rumble_report(std::span<const uint8_t> report);
struct Feedback { uint8_t wcid,large,small_motor; std::optional<std::array<uint8_t,9>> packet{}; };
class InputOutput {
public:
    virtual ~InputOutput()=default;
    virtual void connect(uint8_t wcid)=0;
    virtual void update(uint8_t wcid,const GamepadInput &input,bool guide)=0;
    virtual void disconnect(uint8_t wcid)=0;
    virtual std::optional<Feedback> feedback()=0;
};
class VigemOutput final:public InputOutput {
    struct Impl; std::unique_ptr<Impl> impl_;
public:
    VigemOutput();
    ~VigemOutput();
    void connect(uint8_t wcid) override;
    void update(uint8_t wcid,const GamepadInput &input,bool guide) override;
    void disconnect(uint8_t wcid) override;
    std::optional<Feedback> feedback() override;
    unsigned slot(uint8_t wcid) const;
};
class NativeGipOutput final:public InputOutput {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    NativeGipOutput();
    ~NativeGipOutput();
    void connect(uint8_t wcid) override;
    void update(uint8_t wcid,const GamepadInput &input,bool guide) override;
    void disconnect(uint8_t wcid) override;
    std::optional<Feedback> feedback() override;
};
}

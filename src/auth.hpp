// SPDX-License-Identifier: GPL-2.0-or-later
// Port of xone auth/auth.c and crypto.c, Copyright (C) 2023 Severin von Wnuck-Lipinski.
#pragma once
#include <span>
#include <vector>
#include <array>
#include <functional>
#include <string_view>
#include <string>
#include <stdexcept>
namespace xone {
// Device protocol failures can be isolated from transport/crypto implementation errors.
class AuthProtocolError:public std::runtime_error { public: using std::runtime_error::runtime_error; };
class UnsupportedAuthVersion:public AuthProtocolError {
public:
    explicit UnsupportedAuthVersion(uint8_t version):AuthProtocolError("Unsupported authentication version "+std::to_string(version)){}
};
using Bytes=std::vector<uint8_t>;
Bytes hash256(std::span<const uint8_t> data);
Bytes auth_prf(std::string_view label,std::span<const uint8_t> key,std::span<const uint8_t> seed,size_t length);
// Private blob argument is for deterministic interoperability tests; production generates fresh keys.
Bytes auth_ecdh(std::span<const uint8_t> peer,Bytes &public_key,std::span<const uint8_t> private_blob={});
class Auth {
    Bytes host_random_,client_random_,master_,transcript_;
    uint8_t last_=0,expected_=0;
    bool complete_=false;
    uint8_t version_=0,peer_version_=0;
    std::function<void(std::span<const uint8_t>,bool)> send_;
    std::function<void(std::span<const uint8_t>)> key_;
    void hello(bool v2);
    void send_data(uint8_t command,std::span<const uint8_t> data);
    void request(uint8_t command,uint16_t length);
    void finish(uint8_t command);
    Bytes random_seed() const;
public:
    Auth(std::function<void(std::span<const uint8_t>,bool)> send,std::function<void(std::span<const uint8_t>)> key);
    ~Auth();
    void start();
    void receive(std::span<const uint8_t> data);
    bool complete() const { return complete_; }
    uint8_t version() const { return version_; }
    uint8_t peer_version() const { return peer_version_; }
};
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Port of xone auth/auth.c and crypto.c, Copyright (C) 2023 Severin von Wnuck-Lipinski.
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "auth.hpp"
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <string>
namespace xone {
static void status(NTSTATUS s,const char *operation) { if (s<0) throw std::runtime_error(std::string(operation)+" failed (NTSTATUS="+std::to_string(static_cast<uint32_t>(s))+")"); }
static PUCHAR ptr(std::span<const uint8_t> b) { return const_cast<PUCHAR>(b.data()); }
static ULONG len(std::span<const uint8_t> b) { if (b.size()>65536) throw std::runtime_error("Crypto input too large"); return static_cast<ULONG>(b.size()); }
struct Key { BCRYPT_KEY_HANDLE value=nullptr; ~Key(){ if(value) BCryptDestroyKey(value); } };
struct Secret { BCRYPT_SECRET_HANDLE value=nullptr; ~Secret(){ if(value) BCryptDestroySecret(value); } };
static Bytes random_bytes(size_t n) { Bytes b(n); status(BCryptGenRandom(nullptr,b.data(),len(b),BCRYPT_USE_SYSTEM_PREFERRED_RNG),"Random"); return b; }
Bytes hash256(std::span<const uint8_t> b) { Bytes result(32); status(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,ptr(b),len(b),result.data(),32),"SHA256"); return result; }
static Bytes hmac(std::span<const uint8_t> key,std::span<const uint8_t> data) {
    Bytes result(32); status(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE,ptr(key),len(key),ptr(data),len(data),result.data(),32),"HMAC-SHA256"); return result;
}
Bytes auth_prf(std::string_view label,std::span<const uint8_t> key,std::span<const uint8_t> seed,size_t length) {
    if (length>65535) throw std::runtime_error("PRF output too large");
    Bytes input(label.begin(),label.end()); input.insert(input.end(),seed.begin(),seed.end());
    auto a=hmac(key,input); Bytes out;
    while (out.size()<length) {
        auto block=a; block.insert(block.end(),input.begin(),input.end()); block=hmac(key,block);
        out.insert(out.end(),block.begin(),block.begin()+std::min(block.size(),length-out.size())); a=hmac(key,a);
    }
    return out;
}
Bytes auth_ecdh(std::span<const uint8_t> peer,Bytes &public_key,std::span<const uint8_t> private_blob) {
    if (peer.size()!=64) throw std::runtime_error("Invalid P256 public key size");
    Key local,remote;
    if (private_blob.empty()) {
        status(BCryptGenerateKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE,&local.value,256,0),"Generate ECDH");
        status(BCryptFinalizeKeyPair(local.value,0),"Finalize ECDH");
    } else status(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE,nullptr,BCRYPT_ECCPRIVATE_BLOB,&local.value,ptr(private_blob),len(private_blob),0),"Import private ECDH");
    BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDH_PUBLIC_P256_MAGIC,32};
    Bytes blob(sizeof(header)); std::memcpy(blob.data(),&header,sizeof(header)); blob.insert(blob.end(),peer.begin(),peer.end());
    status(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE,nullptr,BCRYPT_ECCPUBLIC_BLOB,&remote.value,blob.data(),len(blob),0),"Import peer ECDH");
    Bytes exported(72); ULONG actual=0;
    status(BCryptExportKey(local.value,nullptr,BCRYPT_ECCPUBLIC_BLOB,exported.data(),len(exported),&actual,0),"Export ECDH");
    if (actual!=72) throw std::runtime_error("Unexpected ECDH key layout");
    public_key.assign(exported.begin()+8,exported.end()); Secret secret;
    status(BCryptSecretAgreement(local.value,remote.value,&secret.value,0),"ECDH agreement");
    Bytes raw(32); status(BCryptDeriveKey(secret.value,BCRYPT_KDF_RAW_SECRET,nullptr,raw.data(),32,&actual,0),"Derive ECDH");
    if (actual!=32) throw std::runtime_error("Unexpected ECDH secret length");
    // CNG RAW_SECRET is little endian. Linux crypto_kpp returns the big endian x coordinate.
    std::reverse(raw.begin(),raw.end()); auto result=hash256(raw); SecureZeroMemory(raw.data(),raw.size()); return result;
}
static Bytes encrypt_rsa(std::span<const uint8_t> certificate,std::span<const uint8_t> pms) {
    const std::array<uint8_t,4> marker{0x30,0x82,1,0x0a};
    const auto at=std::search(certificate.begin(),certificate.end(),marker.begin(),marker.end());
    if (at==certificate.end() || certificate.end()-at<270) throw std::runtime_error("RSA key missing from certificate");
    const auto der=std::span<const uint8_t>(&*at,270);
    // Fixed RSA-2048 PKCS#1 public key used by xone: SEQUENCE, INTEGER(257), INTEGER(3).
    if (der[4]!=2 || der[5]!=0x82 || der[6]!=1 || der[7]!=1 || der[8]!=0 || der[265]!=2 || der[266]!=3)
        throw std::runtime_error("Unsupported RSA key encoding");
    BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPUBLIC_MAGIC,2048,3,256,0,0};
    Bytes blob(sizeof(header)); std::memcpy(blob.data(),&header,sizeof(header));
    blob.insert(blob.end(),der.begin()+267,der.end()); blob.insert(blob.end(),der.begin()+9,der.begin()+265);
    Key key; status(BCryptImportKeyPair(BCRYPT_RSA_ALG_HANDLE,nullptr,BCRYPT_RSAPUBLIC_BLOB,&key.value,blob.data(),len(blob),0),"Import RSA");
    Bytes cipher(256); ULONG actual=0;
    status(BCryptEncrypt(key.value,ptr(pms),len(pms),nullptr,nullptr,0,cipher.data(),256,&actual,BCRYPT_PAD_PKCS1),"Encrypt premaster");
    if (actual!=256) throw std::runtime_error("Unexpected RSA ciphertext length"); return cipher;
}
Auth::Auth(std::function<void(std::span<const uint8_t>,bool)> send,std::function<void(std::span<const uint8_t>)> key):send_(std::move(send)),key_(std::move(key)) {}
Auth::~Auth() { if (!master_.empty()) SecureZeroMemory(master_.data(),master_.size()); }
Bytes Auth::random_seed() const { auto b=host_random_; b.insert(b.end(),client_random_.begin(),client_random_.end()); return b; }
void Auth::send_data(uint8_t cmd,std::span<const uint8_t> data) {
    const auto n=static_cast<uint16_t>(data.size()); const auto total=static_cast<uint16_t>(n+4);
    Bytes p{0,0x41,0,cmd,static_cast<uint8_t>(total>>8),static_cast<uint8_t>(total),cmd,static_cast<uint8_t>(cmd>=0x21?2:1),static_cast<uint8_t>(n>>8),static_cast<uint8_t>(n)};
    p.insert(p.end(),data.begin(),data.end()); transcript_.insert(transcript_.end(),p.begin()+6,p.end()); p.resize(p.size()+8);
    last_=cmd; send_(p,true);
}
void Auth::request(uint8_t cmd,uint16_t n) {
    expected_=cmd; n+=4;
    Bytes p{0,0x42,0,cmd,static_cast<uint8_t>(n>>8),static_cast<uint8_t>(n)}; p.resize(14); send_(p,true);
}
void Auth::hello(bool v2) { version_=v2?2:1;host_random_=random_bytes(32); auto data=host_random_; data.resize(v2?36:40); send_data(v2?0x21:1,data); }
void Auth::start() { transcript_.clear(); master_.clear(); client_random_.clear(); complete_=false; expected_=0; peer_version_=0;hello(false); }
void Auth::finish(uint8_t cmd) { send_data(cmd,auth_prf("Host Finished",master_,hash256(transcript_),32)); }
void Auth::receive(std::span<const uint8_t> p) {
    if (complete_) return;
    if (p.size()<6 || p[0]!=0) throw AuthProtocolError("Invalid auth handshake header");
    if(p[2])throw AuthProtocolError("Device authentication error "+std::to_string(p[2])+" for command "+std::to_string(p[3]));
    if (p[1]&1) {
        if (p[3]!=1) throw AuthProtocolError("Device rejected auth handshake");
        switch(last_) {
        case 1: request(2,80); break; case 0x21: request(0x22,172); break;
        case 5: finish(7); break; case 0x25: finish(0x26); break;
        case 7: request(8,64); break; case 0x26: request(0x27,64); break;
        default: throw AuthProtocolError("Unexpected auth ACK");
        }
        return;
    }
    if (p.size()<10) throw AuthProtocolError("Truncated auth data header");
    if (p[3]!=p[6]) {
        // Actual device reply: outer command 0x22, inner command 0x02/version 2.
        // Upstream detects this mismatch while the v1 client hello is pending.
        if (last_!=1 || expected_!=2 || p[3]!=0x22 || p[6]!=2 || p[7]!=2)
            throw AuthProtocolError("Unexpected auth protocol upgrade");
        transcript_.clear(); expected_=0; hello(true); return;
    }
    const size_t length=(size_t(p[8])<<8)|p[9];
    if (p[6]!=expected_ || length>p.size()-10 || ((size_t(p[4])<<8)|p[5])!=length+4)
        throw AuthProtocolError("Unexpected auth response or length");
    peer_version_=p[7];
    if(peer_version_!=version_){
        // 045e:0b32 advertises version 3 in an empty v1 Client Hello. Probe the
        // implemented v2 exchange once; this does not implement or accept v3.
        if(peer_version_==3 && length==0 && p[6]==2 && last_==1 && expected_==2){
            transcript_.clear();expected_=0;hello(true);return;
        }
        throw UnsupportedAuthVersion(peer_version_);
    }
    const auto data=p.subspan(10,length); const auto cmd=p[6];
    if (cmd==8 || cmd==0x27) {
        if (data.size()<64 || master_.size()!=48) throw AuthProtocolError("Truncated auth finish");
        const auto wanted=auth_prf("Device Finished",master_,hash256(transcript_),32);
        uint8_t mismatch=0; for(size_t i=0;i<32;++i) mismatch|=wanted[i]^data[i];
        if(mismatch) throw AuthProtocolError("Auth transcript mismatch");
        const auto key=auth_prf("EXPORTER DAWN data channel session key for controller",master_,random_seed(),16);
        const std::array<uint8_t,2> done{1,0}; send_(done,false); key_(key); complete_=true; return;
    }
    // Linux schedules RSA/ECDH work after appending this response to the transcript.
    if (transcript_.size()+p.size()>16384) throw AuthProtocolError("Auth transcript exceeds bound");
    transcript_.insert(transcript_.end(),p.begin()+6,p.end());
    switch(cmd) {
    case 2: case 0x22:
        if(data.size()<(cmd==2?80u:172u)) throw AuthProtocolError("Truncated client hello: command="+std::to_string(cmd)+" version="+std::to_string(p[7])+" length="+std::to_string(data.size()));
        client_random_.assign(data.begin(),data.begin()+32); request(cmd==2?3:0x23,cmd==2?1024:768); break;
    case 3: {
        if(data.size()>1024) throw AuthProtocolError("Certificate exceeds bound");
        auto pms=random_bytes(48); const auto encrypted=encrypt_rsa(data,pms);
        master_=auth_prf("Master Secret",pms,random_seed(),48); SecureZeroMemory(pms.data(),pms.size()); send_data(5,encrypted); break;
    }
    case 0x23:
        if(data.size()<768) throw AuthProtocolError("Truncated auth2 certificate"); request(0x24,128); break;
    case 0x24: {
        if(data.size()<128) throw AuthProtocolError("Truncated auth2 public key");
        Bytes public_key; auto secret=auth_ecdh(data.first(64),public_key); master_=auth_prf("Master Secret",secret,random_seed(),48);
        SecureZeroMemory(secret.data(),secret.size()); send_data(0x25,public_key); break;
    }
    default: throw AuthProtocolError("Unsupported auth command");
    }
}
}

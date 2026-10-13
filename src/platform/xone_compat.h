/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef int gfp_t;
#define __packed
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define ETH_ALEN 6
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#define BIT(n) (UINT32_C(1) << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
static inline unsigned xone_mask_shift(u32 mask) {
    unsigned n = 0;
    while (mask && !(mask & 1)) { mask >>= 1; ++n; }
    return n;
}
#define FIELD_PREP(mask,val) (((u32)(val) << xone_mask_shift(mask)) & (mask))
#define FIELD_GET(mask,val) (((u32)(val) & (mask)) >> xone_mask_shift(mask))
#define min_t(type,a,b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define round_up(n,a) (((n)+(a)-1)/(a)*(a))
#define roundup(n,a) round_up(n,a)
/* Only little-endian Windows x64 is currently supported. */
#define cpu_to_le16(n) ((u16)(n))
#define cpu_to_le32(n) ((u32)(n))
#define le16_to_cpu(n) ((u16)(n))
#define le32_to_cpu(n) ((u32)(n))
static inline void put_unaligned_le32(u32 value, void *p) { memcpy(p, &value, 4); }
static inline u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }

struct usb_device {
    void *context;
    int (*control)(void *, u8, u8, u16, u16, void *, u16, unsigned);
    int (*bulk)(void *, u8, void *, int, int *, unsigned);
    int sticky_error;
};
struct device {
    const u8 *firmware_data;
    size_t firmware_size;
    bool verbose;
};
struct firmware { const u8 *data; size_t size; };
#define USB_DIR_IN 0x80
#define USB_DIR_OUT 0x00
#define USB_TYPE_VENDOR 0x40
#define usb_rcvctrlpipe(dev,ep) 0x80
#define usb_sndctrlpipe(dev,ep) 0x00
#define usb_sndbulkpipe(dev,ep) (ep)
int usb_control_msg(struct usb_device *, unsigned, u8, u8, u16, u16, void *, u16, unsigned);
int usb_bulk_msg(struct usb_device *, unsigned, void *, int, int *, unsigned);
int request_firmware(const struct firmware **, const char *, struct device *);
void release_firmware(const struct firmware *);
void msleep(unsigned);
void usleep_range(unsigned, unsigned);
void xone_log(struct device *, bool error, const char *, ...);
#define dev_dbg(dev,...) xone_log(dev,false,__VA_ARGS__)
#define dev_err(dev,...) xone_log(dev,true,__VA_ARGS__)

struct sk_buff { u8 *base; u8 *data; size_t capacity; unsigned len; };
struct sk_buff *alloc_skb(size_t, gfp_t);
void skb_reserve(struct sk_buff *, size_t);
void *skb_push(struct sk_buff *, size_t);
void *skb_put(struct sk_buff *, size_t);
void skb_put_data(struct sk_buff *, const void *, size_t);
void skb_put_u8(struct sk_buff *, u8);
void consume_skb(struct sk_buff *);
#define kfree_skb consume_skb

#define IEEE80211_FTYPE_MGMT 0x0000
#define IEEE80211_STYPE_ASSOC_RESP 0x0010
#define IEEE80211_STYPE_BEACON 0x0080
#define IEEE80211_HT_MPDU_DENSITY_4 5
#pragma pack(push, 1)
struct ieee80211_hdr_3addr {
    u16 frame_control, duration_id;
    u8 addr1[6], addr2[6], addr3[6];
    u16 seq_ctrl;
};
struct ieee80211_mgmt {
    u16 frame_control, duration;
    u8 da[6], sa[6], bssid[6];
    u16 seq_ctrl;
    union {
        struct { uint64_t timestamp; u16 beacon_int, capab_info; } beacon;
        struct { u16 capab_info, status_code, aid; } assoc_resp;
    } u;
};
#pragma pack(pop)
static inline void eth_broadcast_addr(u8 *addr) { memset(addr, 0xff, 6); }
#ifdef __cplusplus
}
#endif

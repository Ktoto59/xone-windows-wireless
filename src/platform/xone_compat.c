/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "xone_compat.h"

int usb_control_msg(struct usb_device *dev, unsigned pipe, u8 request, u8 type,
                    u16 value, u16 index, void *data, u16 length, unsigned timeout) {
    int result;
    (void)pipe;
    if (dev->sticky_error) return dev->sticky_error;
    result = dev->control(dev->context, request, type, value, index, data, length, timeout);
    if (result != length) dev->sticky_error = result < 0 ? result : -EREMOTEIO;
    return dev->sticky_error ? dev->sticky_error : result;
}
int usb_bulk_msg(struct usb_device *dev, unsigned pipe, void *data, int length,
                 int *actual, unsigned timeout) {
    int transferred = 0, result;
    if (dev->sticky_error) return dev->sticky_error;
    result = dev->bulk(dev->context, (u8)pipe, data, length, &transferred, timeout);
    if (!result && transferred != length) result = -EREMOTEIO;
    if (actual) *actual = transferred;
    if (result) dev->sticky_error = result;
    return result;
}
int request_firmware(const struct firmware **out, const char *name, struct device *dev) {
    struct firmware *fw;
    (void)name;
    if (!dev->firmware_data) return -ENOENT;
    fw = malloc(sizeof(*fw));
    if (!fw) return -ENOMEM;
    fw->data = dev->firmware_data;
    fw->size = dev->firmware_size;
    *out = fw;
    return 0;
}
void release_firmware(const struct firmware *fw) { free((void *)fw); }
void msleep(unsigned ms) { Sleep(ms); }
void usleep_range(unsigned low, unsigned high) { (void)high; Sleep((low+999)/1000); }
void xone_log(struct device *dev, bool error, const char *fmt, ...) {
    va_list args;
    if (!error && !dev->verbose) return;
    va_start(args, fmt);
    vfprintf(error ? stderr : stdout, fmt, args);
    va_end(args);
}
static void skb_bounds(bool valid) {
    if (!valid) { fputs("MT76 buffer bounds violated\n", stderr); abort(); }
}
struct sk_buff *alloc_skb(size_t capacity, gfp_t flags) {
    struct sk_buff *skb;
    (void)flags;
    if (capacity > 4 * 1024 * 1024) return NULL;
    skb = calloc(1, sizeof(*skb));
    if (!skb) return NULL;
    skb->base = malloc(capacity ? capacity : 1);
    if (!skb->base) { free(skb); return NULL; }
    skb->data = skb->base;
    skb->capacity = capacity;
    return skb;
}
void skb_reserve(struct sk_buff *skb, size_t n) {
    skb_bounds(!skb->len && n <= skb->capacity - (size_t)(skb->data-skb->base));
    skb->data += n;
}
void *skb_push(struct sk_buff *skb, size_t n) {
    skb_bounds(n <= (size_t)(skb->data-skb->base));
    skb->data -= n;
    skb->len += (unsigned)n;
    return skb->data;
}
void *skb_put(struct sk_buff *skb, size_t n) {
    u8 *out;
    skb_bounds(n <= skb->capacity-(size_t)(skb->data-skb->base)-skb->len);
    out = skb->data + skb->len;
    skb->len += (unsigned)n;
    return out;
}
void skb_put_data(struct sk_buff *skb, const void *p, size_t n) {
    void *out = skb_put(skb, n);
    if (n) memcpy(out, p, n);
}
void skb_put_u8(struct sk_buff *skb, u8 value) { *(u8 *)skb_put(skb, 1) = value; }
void consume_skb(struct sk_buff *skb) { if (skb) { free(skb->base); free(skb); } }

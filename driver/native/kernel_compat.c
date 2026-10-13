/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kernel implementation of the existing xone MT76 portability interface. */
#include <ntddk.h>
#include <stdarg.h>
#include "xone_native.h"

int usb_control_msg(struct usb_device *dev, unsigned pipe, u8 request, u8 type,
                    u16 value, u16 index, void *data, u16 length, unsigned timeout) {
    int result;
    UNREFERENCED_PARAMETER(pipe);
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
    UNREFERENCED_PARAMETER(name);
    if (!dev->firmware_data) return -ENOENT;
    fw = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*fw), XONE_POOL_TAG);
    if (!fw) return -ENOMEM;
    fw->data = dev->firmware_data; fw->size = dev->firmware_size; *out = fw;
    return 0;
}
void release_firmware(const struct firmware *fw) {
    if (fw) ExFreePoolWithTag((void *)fw, XONE_POOL_TAG);
}
void msleep(unsigned ms) {
    LARGE_INTEGER interval;
    interval.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
void usleep_range(unsigned low, unsigned high) {
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(high);
    interval.QuadPart = -(LONGLONG)low * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
void xone_log(struct device *dev, bool error, const char *format, ...) {
    va_list args;
    if (!error && !dev->verbose) return;
    va_start(args, format);
    vDbgPrintEx(DPFLTR_IHVNETWORK_ID, error ? DPFLTR_ERROR_LEVEL : DPFLTR_TRACE_LEVEL, format, args);
    va_end(args);
}
static void skb_bounds(bool valid) {
    /* Internal MT76 builders run at PASSIVE_LEVEL under the initialization SEH
       boundary. Fail initialization rather than copying outside kernel memory. */
    if (!valid) ExRaiseStatus(STATUS_INVALID_BUFFER_SIZE);
}
struct sk_buff *alloc_skb(size_t capacity, gfp_t flags) {
    struct sk_buff *skb;
    UNREFERENCED_PARAMETER(flags);
    if (capacity > XONE_MAX_FIRMWARE) return NULL;
    /* One allocation gives the owner one object to release. */
    skb = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*skb) + (capacity ? capacity : 1), XONE_POOL_TAG);
    if (!skb) return NULL;
    skb->base = (u8 *)(skb + 1); skb->data = skb->base; skb->capacity = capacity;
    return skb;
}
void skb_reserve(struct sk_buff *skb, size_t n) {
    skb_bounds(!skb->len && n <= skb->capacity - (size_t)(skb->data - skb->base));
    skb->data += n;
}
void *skb_push(struct sk_buff *skb, size_t n) {
    skb_bounds(n <= (size_t)(skb->data - skb->base));
    skb->data -= n; skb->len += (unsigned)n; return skb->data;
}
void *skb_put(struct sk_buff *skb, size_t n) {
    u8 *out;
    skb_bounds(n <= skb->capacity - (size_t)(skb->data - skb->base) - skb->len);
    out = skb->data + skb->len; skb->len += (unsigned)n; return out;
}
void skb_put_data(struct sk_buff *skb, const void *p, size_t n) {
    void *out = skb_put(skb, n);
    if (n) memcpy(out, p, n);
}
void skb_put_u8(struct sk_buff *skb, u8 value) { *(u8 *)skb_put(skb, 1) = value; }
void consume_skb(struct sk_buff *skb) { if (skb) ExFreePoolWithTag(skb, XONE_POOL_TAG); }

/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <ndis.h>
#include <usb.h>
#include <usbdlib.h>
#include "../../src/mt76/mt76.h"

#define XONE_POOL_TAG 'noXX'
#define XONE_MAX_FIRMWARE (4u * 1024u * 1024u)
#define XONE_IF_TYPE 281u

/* The kernel owns the physical USB device. No WinUSB, Python or USB/IP here. */
typedef struct _XONE_ADAPTER {
    NDIS_HANDLE Miniport;
    PDEVICE_OBJECT LowerDevice;
    USBD_CONFIGURATION_HANDLE Configuration;
    USBD_PIPE_HANDLE BulkOut, CommandIn, WlanIn;
    struct usb_device Usb;
    struct device FirmwareDevice;
    struct xone_mt76 Radio;
    UCHAR *Firmware;
    volatile LONG Paused, Removed;
} XONE_ADAPTER;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS XoneUsbStart(XONE_ADAPTER *Adapter);
_IRQL_requires_(PASSIVE_LEVEL)
VOID XoneUsbStop(XONE_ADAPTER *Adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS XoneRadioStart(XONE_ADAPTER *Adapter);
VOID XoneRadioRelease(XONE_ADAPTER *Adapter);

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xone_native.h"
#include <usbioctl.h>

static NTSTATUS XoneUrbComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
    UNREFERENCED_PARAMETER(device); UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Bootstrap operations only: all calls are synchronous at PASSIVE_LEVEL. A
   timed-out IRP is cancelled and joined before its URB/buffer can be released. */
static NTSTATUS XoneSubmitUrb(XONE_ADAPTER *adapter, PURB urb, ULONG timeoutMs) {
    KEVENT event;
    LARGE_INTEGER timeout;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NTSTATUS status, waited;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (InterlockedCompareExchange(&adapter->Removed, 0, 0)) return STATUS_DEVICE_REMOVED;
    irp = IoAllocateIrp(adapter->LowerDevice->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, XoneUrbComplete, &event, TRUE, TRUE, TRUE);
    status = IoCallDriver(adapter->LowerDevice, irp);
    timeout.QuadPart = -(LONGLONG)timeoutMs * 10000;
    waited = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE,
                                  status == STATUS_PENDING ? &timeout : NULL);
    if (waited == STATUS_TIMEOUT) {
        IoCancelIrp(irp);
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = STATUS_IO_TIMEOUT;
    } else {
        status = irp->IoStatus.Status;
        if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status)) status = STATUS_DEVICE_DATA_ERROR;
    }
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS XoneDescriptor(XONE_ADAPTER *adapter, UCHAR type, PVOID buffer, ULONG length) {
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb = {0};
    NTSTATUS status;
    urb.Hdr.Length = sizeof(urb); urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = length; urb.DescriptorType = type;
    status = XoneSubmitUrb(adapter, (PURB)&urb, 1000);
    if (NT_SUCCESS(status) && urb.TransferBufferLength != length) return STATUS_DEVICE_DATA_ERROR;
    return status;
}

static int XoneControl(void *context, u8 request, u8 type, u16 value, u16 index,
                       void *buffer, u16 length, unsigned timeout) {
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb = {0};
    NTSTATUS status;
    urb.Hdr.Length = sizeof(urb); urb.Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    urb.TransferFlags = (type & USB_DIR_IN) ? USBD_TRANSFER_DIRECTION_IN : 0;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = length;
    urb.RequestTypeReservedBits = type & 0x1f;
    urb.Request = request; urb.Value = value; urb.Index = index;
    status = XoneSubmitUrb(context, (PURB)&urb, timeout);
    return NT_SUCCESS(status) ? (int)urb.TransferBufferLength : -EIO;
}

static int XoneBulk(void *context, u8 endpoint, void *buffer, int length, int *actual, unsigned timeout) {
    XONE_ADAPTER *adapter = context;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER urb = {0};
    NTSTATUS status;
    if (length < 0 || length > 65536) return -EINVAL;
    switch (endpoint) {
    case 0x04: urb.PipeHandle = adapter->BulkOut; break;
    case 0x84: urb.PipeHandle = adapter->WlanIn; break;
    case 0x85: urb.PipeHandle = adapter->CommandIn; break;
    default: return -EINVAL;
    }
    if (!urb.PipeHandle) return -ENODEV;
    urb.Hdr.Length = sizeof(urb); urb.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    urb.TransferFlags = (endpoint & 0x80) ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = (ULONG)length;
    status = XoneSubmitUrb(adapter, (PURB)&urb, timeout);
    *actual = NT_SUCCESS(status) ? (int)urb.TransferBufferLength : 0;
    return NT_SUCCESS(status) ? 0 : -EIO;
}

NTSTATUS XoneUsbStart(XONE_ADAPTER *adapter) {
    USB_DEVICE_DESCRIPTOR device = {0};
    USB_CONFIGURATION_DESCRIPTOR header = {0};
    PUSB_CONFIGURATION_DESCRIPTOR config = NULL;
    USBD_INTERFACE_LIST_ENTRY interfaces[2] = {0};
    PURB urb = NULL;
    NTSTATUS status;
    ULONG i;
    status = XoneDescriptor(adapter, USB_DEVICE_DESCRIPTOR_TYPE, &device, sizeof(device));
    if (!NT_SUCCESS(status)) return status;
    if (device.bLength != sizeof(device) || device.idVendor != 0x045e || device.idProduct != 0x02fe)
        return STATUS_NOT_SUPPORTED;
    status = XoneDescriptor(adapter, USB_CONFIGURATION_DESCRIPTOR_TYPE, &header, sizeof(header));
    if (!NT_SUCCESS(status)) return status;
    if (header.bLength != sizeof(header) || header.wTotalLength < sizeof(header))
        return STATUS_DEVICE_DATA_ERROR;
    config = ExAllocatePool2(POOL_FLAG_NON_PAGED, header.wTotalLength, XONE_POOL_TAG);
    if (!config) return STATUS_INSUFFICIENT_RESOURCES;
    status = XoneDescriptor(adapter, USB_CONFIGURATION_DESCRIPTOR_TYPE, config, header.wTotalLength);
    if (!NT_SUCCESS(status)) goto done;
    if (config->wTotalLength != header.wTotalLength || config->bNumInterfaces != 1) {
        status = STATUS_DEVICE_DATA_ERROR; goto done;
    }
    interfaces[0].InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(config, config, 0, 0, -1, -1, -1);
    if (!interfaces[0].InterfaceDescriptor) { status = STATUS_DEVICE_DATA_ERROR; goto done; }
    urb = USBD_CreateConfigurationRequestEx(config, interfaces);
    if (!urb) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    status = XoneSubmitUrb(adapter, urb, 1000);
    if (!NT_SUCCESS(status)) goto done;
    adapter->Configuration = urb->UrbSelectConfiguration.ConfigurationHandle;
    for (i = 0; i < interfaces[0].Interface->NumberOfPipes; ++i) {
        PUSBD_PIPE_INFORMATION pipe = &interfaces[0].Interface->Pipes[i];
        if (pipe->PipeType != UsbdPipeTypeBulk) continue;
        switch (pipe->EndpointAddress) {
        case 0x04: adapter->BulkOut = pipe->PipeHandle; break;
        case 0x84: adapter->WlanIn = pipe->PipeHandle; break;
        case 0x85: adapter->CommandIn = pipe->PipeHandle; break;
        }
    }
    if (!adapter->BulkOut || !adapter->WlanIn || !adapter->CommandIn) {
        status = STATUS_DEVICE_DATA_ERROR; goto done;
    }
    adapter->Usb.context = adapter;
    adapter->Usb.control = XoneControl; adapter->Usb.bulk = XoneBulk;
done:
    if (urb) ExFreePool(urb);
    ExFreePoolWithTag(config, XONE_POOL_TAG);
    return status;
}

VOID XoneUsbStop(XONE_ADAPTER *adapter) {
    if (adapter->Configuration && !adapter->Removed) {
        struct _URB_SELECT_CONFIGURATION urb = {0};
        urb.Hdr.Length = sizeof(urb); urb.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
        (void)XoneSubmitUrb(adapter, (PURB)&urb, 1000);
    }
    adapter->Configuration = NULL;
    adapter->BulkOut = adapter->CommandIn = adapter->WlanIn = NULL;
}

NTSTATUS XoneRadioStart(XONE_ADAPTER *adapter) {
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\SystemRoot\\System32\\drivers\\FW_ACC_00U.bin");
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION information;
    HANDLE file = NULL;
    NTSTATUS status;
    ULONG length, ilm, dlm;
    LARGE_INTEGER offset = {0};
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&file, FILE_READ_DATA | SYNCHRONIZE, &attributes, &io, NULL,
                          FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) return status;
    status = ZwQueryInformationFile(file, &io, &information, sizeof(information), FileStandardInformation);
    if (!NT_SUCCESS(status)) goto done;
    if (information.EndOfFile.QuadPart < 32 || information.EndOfFile.QuadPart > XONE_MAX_FIRMWARE) {
        status = STATUS_INVALID_IMAGE_FORMAT; goto done;
    }
    length = (ULONG)information.EndOfFile.QuadPart;
    adapter->Firmware = ExAllocatePool2(POOL_FLAG_NON_PAGED, length, XONE_POOL_TAG);
    if (!adapter->Firmware) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    status = ZwReadFile(file, NULL, NULL, NULL, &io, adapter->Firmware, length, &offset, NULL);
    if (!NT_SUCCESS(status)) goto done;
    if (io.Information != length) { status = STATUS_END_OF_FILE; goto done; }
    RtlCopyMemory(&ilm, adapter->Firmware, sizeof(ilm));
    RtlCopyMemory(&dlm, adapter->Firmware + 4, sizeof(dlm));
    if (!ilm || !dlm || (ULONGLONG)ilm + dlm + 32 != length) {
        status = STATUS_INVALID_IMAGE_FORMAT; goto done;
    }
    adapter->FirmwareDevice.firmware_data = adapter->Firmware;
    adapter->FirmwareDevice.firmware_size = length;
    adapter->Radio.dev = &adapter->FirmwareDevice; adapter->Radio.udev = &adapter->Usb;
    __try {
        if (xone_mt76_load_firmware(&adapter->Radio, "FW_ACC_00U.bin") || adapter->Usb.sticky_error ||
            xone_mt76_init_radio(&adapter->Radio) || adapter->Usb.sticky_error)
            status = STATUS_DEVICE_HARDWARE_ERROR;
    } __except(EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
done:
    ZwClose(file);
    return status;
}

VOID XoneRadioRelease(XONE_ADAPTER *adapter) {
    if (adapter->Firmware) ExFreePoolWithTag(adapter->Firmware, XONE_POOL_TAG);
    adapter->Firmware = NULL;
    adapter->FirmwareDevice.firmware_data = NULL;
    adapter->FirmwareDevice.firmware_size = 0;
}

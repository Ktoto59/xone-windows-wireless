/* SPDX-License-Identifier: GPL-2.0-or-later */
/* First native adapter bootstrap. GIP packet/OID data plane is not wired yet. */
#include "xone_native.h"

static NDIS_HANDLE XoneDriverHandle;
DRIVER_INITIALIZE DriverEntry;
static MINIPORT_INITIALIZE XoneInitialize;
static MINIPORT_HALT XoneHalt;
static MINIPORT_UNLOAD XoneUnload;
static MINIPORT_PAUSE XonePause;
static MINIPORT_RESTART XoneRestart;
static MINIPORT_OID_REQUEST XoneOidRequest;
static MINIPORT_SEND_NET_BUFFER_LISTS XoneSend;
static MINIPORT_RETURN_NET_BUFFER_LISTS XoneReturn;
static MINIPORT_CANCEL_SEND XoneCancelSend;
static MINIPORT_CANCEL_OID_REQUEST XoneCancelOid;
static MINIPORT_DEVICE_PNP_EVENT_NOTIFY XonePnp;
static MINIPORT_SHUTDOWN XoneShutdown;

static const NDIS_OID XoneSupportedOids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_MULTICAST_LIST,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER, 0xfa010203u /* observed OID_XBOX_ACC_MAC_ADDRESS */
};

static NDIS_STATUS XoneInitialize(NDIS_HANDLE handle, NDIS_HANDLE context,
                                  PNDIS_MINIPORT_INIT_PARAMETERS parameters) {
    XONE_ADAPTER *adapter;
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES registration = {0};
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES general = {0};
    NDIS_STATUS result;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(parameters);
    adapter = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*adapter), XONE_POOL_TAG);
    if (!adapter) return NDIS_STATUS_RESOURCES;
    adapter->Miniport = handle; adapter->Paused = TRUE;
    NdisMGetDeviceProperty(handle, NULL, NULL, &adapter->LowerDevice, NULL, NULL);
    if (!adapter->LowerDevice) { result = NDIS_STATUS_FAILURE; goto failed; }
    registration.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    registration.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.MiniportAdapterContext = adapter;
    registration.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK;
    registration.InterfaceType = NdisInterfacePNPBus;
    result = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&registration);
    if (result != NDIS_STATUS_SUCCESS) goto failed;
    status = XoneUsbStart(adapter);
    if (!NT_SUCCESS(status)) { result = NDIS_STATUS_ADAPTER_NOT_FOUND; goto failed; }
    status = XoneRadioStart(adapter);
    if (!NT_SUCCESS(status)) { result = NDIS_STATUS_ADAPTER_NOT_READY; goto failed; }
    general.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    general.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    general.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    general.MediaType = NdisMedium802_3;
    general.PhysicalMediumType = NdisPhysicalMediumOther;
    general.MtuSize = 1600;
    general.MaxXmitLinkSpeed = general.MaxRcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    general.XmitLinkSpeed = general.RcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    /* Radio initialization is not a connected GIP peer. */
    general.MediaConnectState = MediaConnectStateDisconnected;
    general.MediaDuplexState = MediaDuplexStateUnknown;
    general.LookaheadSize = general.MtuSize;
    general.MacOptions = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    general.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST;
    general.MacAddressLength = 6;
    RtlCopyMemory(general.PermanentMacAddress, adapter->Radio.address, 6);
    RtlCopyMemory(general.CurrentMacAddress, adapter->Radio.address, 6);
    general.AccessType = NET_IF_ACCESS_BROADCAST;
    general.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    general.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    general.IfType = XONE_IF_TYPE;
    general.IfConnectorPresent = TRUE;
    general.SupportedOidList = (PNDIS_OID)XoneSupportedOids;
    general.SupportedOidListLength = sizeof(XoneSupportedOids);
    general.AutoNegotiationFlags = NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED |
                                   NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED;
    result = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&general);
    if (result != NDIS_STATUS_SUCCESS) goto failed;
    DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
               "xone-native: radio bootstrap complete; GIP data plane unavailable\n");
    return NDIS_STATUS_SUCCESS;
failed:
    XoneUsbStop(adapter); XoneRadioRelease(adapter);
    ExFreePoolWithTag(adapter, XONE_POOL_TAG);
    return result;
}

static VOID XoneHalt(NDIS_HANDLE context, NDIS_HALT_ACTION action) {
    XONE_ADAPTER *adapter = context;
    if (action == NdisHaltDeviceSurpriseRemoved) InterlockedExchange(&adapter->Removed, TRUE);
    XoneUsbStop(adapter); XoneRadioRelease(adapter);
    ExFreePoolWithTag(adapter, XONE_POOL_TAG);
}
static VOID XoneUnload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    NdisMDeregisterMiniportDriver(XoneDriverHandle);
}
static NDIS_STATUS XonePause(NDIS_HANDLE context, PNDIS_MINIPORT_PAUSE_PARAMETERS parameters) {
    UNREFERENCED_PARAMETER(parameters);
    InterlockedExchange(&((XONE_ADAPTER *)context)->Paused, TRUE);
    /* Bootstrap phase has no asynchronous receive/send ownership to drain. */
    return NDIS_STATUS_SUCCESS;
}
static NDIS_STATUS XoneRestart(NDIS_HANDLE context, PNDIS_MINIPORT_RESTART_PARAMETERS parameters) {
    XONE_ADAPTER *adapter = context;
    UNREFERENCED_PARAMETER(parameters);
    if (adapter->Removed) return NDIS_STATUS_ADAPTER_REMOVED;
    InterlockedExchange(&adapter->Paused, FALSE);
    return NDIS_STATUS_SUCCESS;
}
static NDIS_STATUS XoneQuery(XONE_ADAPTER *adapter, PNDIS_OID_REQUEST request) {
    ULONG value = 0, length = sizeof(value);
    USHORT version = 0x061e;
    const CHAR description[] = "xone native wireless adapter bootstrap";
    const VOID *data = &value;
    switch (request->DATA.QUERY_INFORMATION.Oid) {
    case OID_GEN_SUPPORTED_LIST: data = XoneSupportedOids; length = sizeof(XoneSupportedOids); break;
    case OID_GEN_HARDWARE_STATUS: value = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: value = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_FRAME_SIZE: case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD: value = 1600; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE: value = 1614; break;
    case OID_GEN_LINK_SPEED: value = 0; break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE: case OID_GEN_RECEIVE_BUFFER_SPACE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE: case OID_GEN_RECEIVE_BLOCK_SIZE: value = 1614; break;
    case OID_GEN_VENDOR_ID: value = 0x00ffffff; break;
    case OID_GEN_VENDOR_DESCRIPTION: data = description; length = sizeof(description); break;
    case OID_GEN_VENDOR_DRIVER_VERSION: value = 0x00000200; break;
    case OID_GEN_DRIVER_VERSION: data = &version; length = sizeof(version); break;
    case OID_GEN_MEDIA_CONNECT_STATUS: value = NdisMediaStateDisconnected; break;
    case OID_802_3_PERMANENT_ADDRESS: case OID_802_3_CURRENT_ADDRESS: case 0xfa010203u:
        data = adapter->Radio.address; length = 6; break;
    case OID_GEN_CURRENT_PACKET_FILTER: case OID_802_3_MAXIMUM_LIST_SIZE:
    case OID_GEN_XMIT_OK: case OID_GEN_RCV_OK: case OID_GEN_XMIT_ERROR:
    case OID_GEN_RCV_ERROR: case OID_GEN_RCV_NO_BUFFER: break;
    case OID_802_3_MULTICAST_LIST: length = 0; break;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    request->DATA.QUERY_INFORMATION.BytesNeeded = length;
    if (request->DATA.QUERY_INFORMATION.InformationBufferLength < length) return NDIS_STATUS_BUFFER_TOO_SHORT;
    if (length) RtlCopyMemory(request->DATA.QUERY_INFORMATION.InformationBuffer, data, length);
    request->DATA.QUERY_INFORMATION.BytesWritten = length;
    return NDIS_STATUS_SUCCESS;
}
static NDIS_STATUS XoneOidRequest(NDIS_HANDLE context, PNDIS_OID_REQUEST request) {
    XONE_ADAPTER *adapter = context;
    if (adapter->Removed) return NDIS_STATUS_ADAPTER_REMOVED;
    if (request->RequestType == NdisRequestQueryInformation || request->RequestType == NdisRequestQueryStatistics) {
        request->DATA.QUERY_INFORMATION.BytesWritten = 0;
        request->DATA.QUERY_INFORMATION.BytesNeeded = 0;
        return XoneQuery(adapter, request);
    }
    /* No successful placeholders for unknown Xbox OIDs, key installation,
       association, packet-filter changes or power requests. */
    if (request->RequestType == NdisRequestSetInformation) {
        request->DATA.SET_INFORMATION.BytesRead = 0;
        request->DATA.SET_INFORMATION.BytesNeeded = 0;
    }
    return NDIS_STATUS_NOT_SUPPORTED;
}
static VOID XoneSend(NDIS_HANDLE context, PNET_BUFFER_LIST lists, NDIS_PORT_NUMBER port, ULONG flags) {
    XONE_ADAPTER *adapter = context;
    PNET_BUFFER_LIST item;
    ULONG completionFlags = (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
    UNREFERENCED_PARAMETER(port);
    for (item = lists; item; item = NET_BUFFER_LIST_NEXT_NBL(item))
        NET_BUFFER_LIST_STATUS(item) = adapter->Removed ? NDIS_STATUS_ADAPTER_REMOVED :
            adapter->Paused ? NDIS_STATUS_PAUSED : NDIS_STATUS_NOT_SUPPORTED;
    NdisMSendNetBufferListsComplete(adapter->Miniport, lists, completionFlags);
}
static VOID XoneReturn(NDIS_HANDLE context, PNET_BUFFER_LIST lists, ULONG flags) {
    /* No receive indications exist in this bootstrap build. */
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(lists); UNREFERENCED_PARAMETER(flags);
}
static VOID XoneCancelSend(NDIS_HANDLE context, PVOID cancelId) {
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(cancelId);
}
static VOID XoneCancelOid(NDIS_HANDLE context, PVOID requestId) {
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(requestId);
}
static VOID XonePnp(NDIS_HANDLE context, PNET_DEVICE_PNP_EVENT event) {
    if (event->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved)
        InterlockedExchange(&((XONE_ADAPTER *)context)->Removed, TRUE);
}
static VOID XoneShutdown(NDIS_HANDLE context, NDIS_SHUTDOWN_ACTION action) {
    UNREFERENCED_PARAMETER(action);
    /* Shutdown may run at high IRQL: no blocking USB calls here. */
    InterlockedExchange(&((XONE_ADAPTER *)context)->Removed, TRUE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS characteristics = {0};
    characteristics.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    characteristics.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    characteristics.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    characteristics.MajorNdisVersion = 6; characteristics.MinorNdisVersion = 30;
    characteristics.MinorDriverVersion = 2;
    characteristics.Flags = NDIS_WDM_DRIVER;
    characteristics.InitializeHandlerEx = XoneInitialize; characteristics.HaltHandlerEx = XoneHalt;
    characteristics.UnloadHandler = XoneUnload; characteristics.PauseHandler = XonePause;
    characteristics.RestartHandler = XoneRestart; characteristics.OidRequestHandler = XoneOidRequest;
    characteristics.SendNetBufferListsHandler = XoneSend; characteristics.ReturnNetBufferListsHandler = XoneReturn;
    characteristics.CancelSendHandler = XoneCancelSend; characteristics.CancelOidRequestHandler = XoneCancelOid;
    characteristics.DevicePnPEventNotifyHandler = XonePnp; characteristics.ShutdownHandlerEx = XoneShutdown;
    return NdisMRegisterMiniportDriver(driver, path, NULL, &characteristics, &XoneDriverHandle);
}

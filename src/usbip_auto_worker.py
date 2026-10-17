# SPDX-License-Identifier: GPL-2.0-or-later
"""One isolated, dynamically identified RF/USBIP device; no hardware ownership."""
import asyncio
from pathlib import Path
import re
import sys
import development_trace as trace
from bridge_telemetry import BridgeTelemetry
from usb_gip_radio import RadioBackend, read_frame
from usbip_reference_server import ReferenceServer
from device_connectors import PROFILES, RADIO_MODELS, model, connectors


def serial_for(mac, pid):
    if not re.fullmatch('[0-9a-f]{12}',mac) or pid not in PROFILES:
        raise ValueError('Invalid dynamic USB identity')
    return f'XONE{pid:04X}{mac.upper()}'


async def serve(sock, root, usb_port, status_file, run_id, preserve_old_wireless_mode=True):
    # Load descriptors once, before servicing GIP. No file I/O in on_announce.
    policies=connectors()
    profiles = {pid:connector.load_profile(root) for pid,connector in policies.items()}
    telemetry = BridgeTelemetry(status_file)
    backend = reference = None
    identity = {'RunId':run_id,'Mac':None,'Pid':None,'RadioPid':None,
                'Profile':None,'Connector':None,'VirtualSerial':None,'RejectedReason':None}
    snapshot_task = None
    server = None
    writer = None
    try:
        sock.setblocking(False)
        reader,writer = await asyncio.open_connection(sock=sock)
        handshake = await read_frame(reader)
        kind,wcid,token,mac_bytes = handshake
        if (kind,wcid,token)!=(0,0,0) or len(mac_bytes)!=6 or not any(mac_bytes) or mac_bytes[0]&1:
            raise ValueError('Invalid automatic RF handshake')
        mac = mac_bytes.hex()
        identity['Mac'] = mac
        reference = ReferenceServer(None, on_event=telemetry.event if trace.active() else None)

        def announce(vendor, radio_product):
            try:
                pid = model(vendor,radio_product)
                if identity['Pid'] is not None and (identity['Pid']!=pid or identity['RadioPid']!=radio_product):
                    raise ValueError('Radio model changed inside an association')
                if identity['Pid'] is not None:
                    return
                profile = profiles[pid]
                profile.virtual_serial = serial_for(mac,pid)
                connector=policies[pid]
                connector.configure(backend,profile,preserve_old_wireless_mode)
                reference.profile = profile
                identity.update(Pid=pid,RadioPid=radio_product,Profile=PROFILES[pid],Connector=connector.config.name,
                                VirtualSerial=profile.virtual_serial)
                telemetry.event(dict(operation='automatic_identity',**identity))
            except ValueError as error:
                identity['RejectedReason'] = str(error)
                raise

        backend = RadioBackend(mac,on_event=telemetry.event,on_announce=announce)
        reference.backend = backend

        async def snapshots():
            while True:
                telemetry.publish(dict(backend.status(),**identity,
                                       Ready=backend.ready and reference.profile is not None,
                                       UsbImported=reference.imported))
                await asyncio.sleep(.5)

        server = await asyncio.start_server(reference.connection,'127.0.0.1',usb_port)
        snapshot_task = asyncio.create_task(snapshots())
        await backend.connection(reader,writer,handshake=handshake)
    except (OSError,ValueError,TimeoutError,asyncio.IncompleteReadError) as error:
        identity['RejectedReason'] = identity['RejectedReason'] or str(error)
        telemetry.event(dict(operation='automatic_worker_closed',reason=str(error)))
    finally:
        if server is not None:
            server.close()
        # A live Windows import must not keep this association's worker alive
        # after RF EOF. Close only this worker's sockets/tasks before asyncio.run
        # performs global cancellation, so its final status is not left stale.
        if reference is not None:
            await reference.close_clients()
        if server is not None:
            await server.wait_closed()
        if snapshot_task is not None:
            snapshot_task.cancel()
            await asyncio.gather(snapshot_task,return_exceptions=True)
        if writer is not None:
            writer.close()
        snapshot = backend.status() if backend is not None else {}
        snapshot.update(identity,Ready=False,UsbImported=False)
        telemetry.publish(snapshot)
        await asyncio.to_thread(telemetry.close)


def worker_main(sock, root, usb_port, status_file, run_id, log_path, error_path, preserve_old_wireless_mode=True):
    # multiprocessing passes a duplicated socket handle, not audio through the
    # supervisor. The parent closes its original immediately after spawn.
    sys.stdout = open(log_path,'w',encoding='utf-8',buffering=1)
    sys.stderr = open(error_path,'w',encoding='utf-8',buffering=1)
    if sys.platform=='win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    try:
        trace.start_from_environment('worker-'+run_id)
        trace.event('worker_started', run_id=run_id, usb_port=usb_port)
        asyncio.run(serve_traced(sock,Path(root),usb_port,Path(status_file),run_id,preserve_old_wireless_mode))
    finally:
        sock.close()
        trace.stop()


async def serve_traced(*args):
    monitor = asyncio.create_task(trace.monitor_loop(), name='development-loop-monitor') if trace.active() else None
    try:
        return await serve(*args)
    finally:
        if monitor is not None:
            monitor.cancel()
            await asyncio.gather(monitor, return_exceptions=True)

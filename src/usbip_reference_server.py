# SPDX-License-Identifier: GPL-2.0-or-later
"""Local USB/IP metadata preview and explicit USB enumeration diagnostic.

Does not bind physical USB, change drivers, or expose a network-wide listener.
"""
import argparse
import asyncio
from pathlib import Path
import struct
import sys
import time
from usb_headset_profile import HeadsetProfile
from usbip_urb import UrbSession
from usb_gip_radio import RadioBackend
from bridge_telemetry import BridgeTelemetry


class EnumerationBackend:
    async def submit(self, request, endpoint):
        if request.direction and endpoint.transfer_type == 3:
            # A real idle interrupt endpoint NAKs until it has a report.
            # Keep the URB pending and cancellable; repeated EPIPE responses
            # otherwise make the native driver continuously reset the pipe.
            await asyncio.Future()
        from usbip_urb import failed_completion
        return failed_completion(request, -32)


def transport_profile(path, audio_bridge=False):
    profile = HeadsetProfile.load(path)
    if audio_bridge:
        # usbip-win2 rewrites Full-Speed ISO bInterval=1 to 4. dc1-controller
        # then packs only one of the native GIP driver's eight audio fragments.
        # A virtual High-Speed presentation preserves bInterval=1 and its
        # eight-slot batching. The physical reference JSON remains unchanged.
        profile.usbip_speed = 3
    return profile


def radio_endpoints(profile):
    interrupt = profile.settings[(0,0)]
    audio = [e for (number,alternate), endpoints in profile.settings.items()
             if alternate for e in endpoints if e.transfer_type == 1]
    def pair(endpoints,kind):
        if len(endpoints) != 2 or any(e.transfer_type != kind for e in endpoints):
            raise ValueError('Expected one captured IN/OUT endpoint pair')
        outgoing = [e.address for e in endpoints if not e.address & 128]
        incoming = [e.address for e in endpoints if e.address & 128]
        if len(outgoing) != 1 or len(incoming) != 1:
            raise ValueError('Endpoint pair direction mismatch')
        return outgoing[0],incoming[0]
    return pair(interrupt,3),pair(audio,1)


def render_budget(profile):
    # The 2021 native USB profile keeps a deeper future-OUT pipeline than
    # 2024: observed ~66 ms under ordinary playback, without slow radio ACKs.
    return .096 if profile.pid == 0x0b17 else .064


class ReferenceServer:
    def __init__(self, profile, timeout=5, enumeration_probe=False, on_event=None, backend=None):
        self.profile = profile
        self.timeout = timeout
        self.clients = 0
        self.enumeration_probe = enumeration_probe
        self.imported = False
        self.on_event = on_event
        self.backend = backend
        self.connection_tasks = set()
        self.connection_writers = set()

    async def close_clients(self):
        """End this device's USB imports when its RF association ends."""
        tasks = tuple(self.connection_tasks)
        for writer in tuple(self.connection_writers):
            writer.transport.abort()
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.wait(tasks,timeout=1)

    async def connection(self, reader, writer):
        if self.clients >= 16:
            writer.close()
            return
        self.clients += 1
        task = asyncio.current_task()
        self.connection_tasks.add(task)
        self.connection_writers.add(writer)
        owns_import = False
        try:
            header = await asyncio.wait_for(reader.readexactly(8), self.timeout)
            version, operation, status = struct.unpack('!HHI', header)
            if version != 0x0111 or status != 0:
                return
            if operation == 0x8005:
                writer.write(self.profile.usbip_device_list() if self.profile is not None else struct.pack('!HHII',0x0111,5,0,0))
            elif operation == 0x8003:
                busid = await asyncio.wait_for(reader.readexactly(32), self.timeout)
                # Import only with an explicit diagnostic mode or a live RF peer.
                available = self.profile is not None and (self.enumeration_probe or (self.backend is not None and self.backend.ready))
                if available and not self.imported and busid == b'1-1'.ljust(32, b'\0'):
                    self.imported = owns_import = True
                    writer.write(struct.pack('!HHI', 0x0111, 3, 0) + self.profile.usbip_device())
                    await asyncio.wait_for(writer.drain(), self.timeout)
                    backend = self.backend if self.backend is not None else EnumerationBackend()
                    await UrbSession(self.profile, backend=backend, timeout=120 if self.backend else max(30, self.timeout), on_event=self.on_event).run(reader, writer)
                    return
                writer.write(struct.pack('!HHI', 0x0111, 3, 1))
            else:
                return
            await asyncio.wait_for(writer.drain(), self.timeout)
        except (asyncio.IncompleteReadError, TimeoutError, ConnectionError):
            pass
        finally:
            self.connection_tasks.discard(task)
            self.connection_writers.discard(writer)
            if owns_import:
                self.imported = False
                if self.backend is not None:
                    self.backend.clear_audio()
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass
            self.clients -= 1


async def wait_for_lifetime(seconds):
    """Zero means no automatic shutdown; cancellation still performs cleanup."""
    if seconds:
        await asyncio.sleep(seconds)
    else:
        await asyncio.Future()


async def main(args):
    telemetry = BridgeTelemetry(args.status_file)
    backend = None
    event = telemetry.event

    async def snapshots():
        maximum_lag = 0
        while True:
            if backend is not None:
                telemetry.publish(dict(backend.status(), EventLoopMaxLagMs=round(maximum_lag*1000, 3)))
            expected = time.monotonic()+.5
            await asyncio.sleep(.5)
            maximum_lag = max(maximum_lag, time.monotonic()-expected)
    profile = transport_profile(args.profile,args.audio_bridge)
    if args.radio_mac:
        interrupt,audio = radio_endpoints(profile)
        backend = RadioBackend(args.radio_mac, on_event=event, audio_probe=args.audio_probe, audio_bridge=args.audio_bridge,
                               interrupt_endpoints=interrupt,audio_endpoints=audio,render_max_age=render_budget(profile),
                               independent_clock=getattr(args,'audio_independent_clock',False))
    reference = ReferenceServer(profile, enumeration_probe=args.enumeration_probe,
                                on_event=event if getattr(args, 'diagnostics', False) else None, backend=backend)
    server = await asyncio.start_server(reference.connection, '127.0.0.1', args.port)
    mode = ('radio GIP; experimental PCM bridge' if args.audio_bridge else
            'radio GIP; capture/header audio probe' if args.audio_probe else
            'radio GIP; ISO audio unavailable') if backend else ('enumeration diagnostic; GIP/audio unavailable' if args.enumeration_probe else 'metadata preview; import disabled')
    event({'operation': 'listening', 'port': server.sockets[0].getsockname()[1], 'mode': mode,
           'lifetime_seconds': args.seconds})
    if args.audio_bridge:
        event({'operation': 'usb_audio_profile', 'usbip_speed': 3, 'iso_interval': 1, 'expected_slots': 8})
    status_task = asyncio.create_task(snapshots())
    try:
        if backend:
            radio_server = await asyncio.start_server(backend.connection, '127.0.0.1', args.radio_port)
            event({'operation': 'radio_waiting', 'port': args.radio_port})
            async with radio_server, server:
                await wait_for_lifetime(args.seconds)
        else:
            async with server:
                await wait_for_lifetime(args.seconds)
    finally:
        status_task.cancel()
        await asyncio.gather(status_task, return_exceptions=True)
        if backend is not None:
            telemetry.publish(dict(backend.status(), Ready=False))
        await asyncio.to_thread(telemetry.close)


if __name__ == '__main__':
    if sys.platform == 'win32':
        # TCP only. Python 3.11's Proactor transport can report an unhandled
        # shutdown() ConnectionResetError when the native USBip peer detaches.
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, default=Path(__file__).resolve().parents[1] / 'driver/profiles/headset-0b32-usb.json')
    parser.add_argument('--port', type=int, default=33240)
    parser.add_argument('--seconds', type=int, default=0, help='Lifetime in seconds; 0 runs until stopped (default)')
    parser.add_argument('--enumeration-probe', action='store_true', help='Diagnostic attachment only; GIP/audio stall, no RF access')
    parser.add_argument('--radio-mac', help='Route interrupt GIP to this radio MAC through xone-probe --output usb-gip')
    parser.add_argument('--radio-port', type=int, default=33241)
    parser.add_argument('--audio-probe', action='store_true', help='Experimental radio capture ISO path and render header diagnostics; playback unsupported')
    parser.add_argument('--audio-bridge', action='store_true', help='Experimental observed 24k mono / 48k stereo radio PCM conversion')
    parser.add_argument('--audio-independent-clock', action='store_true',
                        help='Experimental independent playback and bounded microphone silence concealment; requires audio-bridge')
    parser.add_argument('--status-file', type=Path)
    parser.add_argument('--diagnostics', action='store_true', help='Enable bounded per-URB header diagnostics; off during ordinary use')
    arguments = parser.parse_args()
    if arguments.audio_independent_clock and not arguments.audio_bridge:
        parser.error('audio-independent-clock requires audio-bridge')
    if (arguments.audio_probe or arguments.audio_bridge) and not arguments.radio_mac:
        parser.error('audio options require radio-mac')
    if not 0 <= arguments.port <= 65535 or not 0 <= arguments.seconds <= 3600:
        parser.error('port must be 0..65535; seconds must be 0..3600 (0 runs until stopped)')
    if arguments.radio_mac and (arguments.enumeration_probe or len(arguments.radio_mac) != 12 or any(c not in '0123456789abcdefABCDEF' for c in arguments.radio_mac)):
        parser.error('radio-mac must be twelve hex digits and cannot combine with enumeration-probe')
    if not 1024 <= arguments.radio_port <= 65535 or arguments.radio_port == arguments.port:
        parser.error('radio-port must be 1024..65535 and differ from USB/IP port')
    asyncio.run(main(arguments))

import asyncio
from pathlib import Path
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_headset_profile import HeadsetProfile
from usbip_reference_server import ReferenceServer, transport_profile, main, render_budget
from bridge_telemetry import BridgeTelemetry
from usb_gip_radio import RadioBackend


class LifetimeTests(unittest.IsolatedAsyncioTestCase):
    async def test_zero_lifetime_serves_requests_until_cancelled_and_finite_mode_still_closes(self):
        for seconds in (0, .5):
            with self.subTest(seconds=seconds):
                started = asyncio.Event()
                servers = []
                real_start = asyncio.start_server
                async def start(*args, **kwargs):
                    server = await real_start(*args, **kwargs)
                    servers.append(server)
                    started.set()
                    return server
                args = SimpleNamespace(profile=ROOT / 'driver/profiles/headset-0b32-usb.json',
                    port=0, seconds=seconds, radio_mac=None, radio_port=0,
                    audio_bridge=False, audio_probe=False, enumeration_probe=False, status_file=None)
                with patch('usbip_reference_server.asyncio.start_server', side_effect=start), \
                     patch('usbip_reference_server.BridgeTelemetry', side_effect=lambda path: BridgeTelemetry(path, emit=lambda fields: None)):
                    task = asyncio.create_task(main(args))
                    writer = None
                    try:
                        await asyncio.wait_for(started.wait(), 1)
                        await asyncio.sleep(.02)
                        self.assertFalse(task.done())
                        reader, writer = await asyncio.open_connection('127.0.0.1',servers[0].sockets[0].getsockname()[1])
                        writer.write(bytes.fromhex('0111800500000000'));await writer.drain()
                        self.assertEqual(await asyncio.wait_for(reader.read(), 1), transport_profile(args.profile).usbip_device_list())
                        writer.close();await writer.wait_closed();writer = None
                        if seconds == 0:
                            task.cancel()
                            with self.assertRaises(asyncio.CancelledError):
                                await task
                        else:
                            await asyncio.wait_for(task, 2)
                        self.assertFalse(servers[0].is_serving())
                    finally:
                        if writer is not None:
                            writer.close();await writer.wait_closed()
                        if not task.done():
                            task.cancel()
                        await asyncio.gather(task, return_exceptions=True)


class TransportTests(unittest.IsolatedAsyncioTestCase):
    def test_old_and_new_profiles_keep_independent_render_budgets(self):
        old = transport_profile(ROOT / 'driver/profiles/headset-0b17-usb.json', True)
        new = transport_profile(ROOT / 'driver/profiles/headset-0b32-usb.json', True)
        self.assertEqual((render_budget(old),render_budget(new)),(.096,.064))

    def test_audio_transport_preserves_intervals_without_changing_reference(self):
        path = ROOT / 'driver/profiles/headset-0b32-usb.json'
        reference = transport_profile(path)
        audio = transport_profile(path,True)
        self.assertEqual((reference.usbip_speed,audio.usbip_speed),(2,3))
        self.assertEqual(reference.configuration,audio.configuration)
        self.assertEqual(reference.settings,audio.settings)
        self.assertEqual(HeadsetProfile.load(path).usbip_speed,2)

    async def asyncSetUp(self):
        self.profile = HeadsetProfile.load(ROOT / 'driver/profiles/headset-0b32-usb.json')
        self.handler = ReferenceServer(self.profile, timeout=0.2)
        self.server = await asyncio.start_server(self.handler.connection, '127.0.0.1', 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        self.server.close()
        await self.server.wait_closed()

    async def test_fragmented_request_and_repeated_connections(self):
        for _ in range(3):
            reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
            header = bytes.fromhex('0111800500000000')
            for value in header:
                writer.write(bytes((value,)))
                await writer.drain()
                await asyncio.sleep(0)
            self.assertEqual(await reader.read(), self.profile.usbip_device_list())
            writer.close()
            await writer.wait_closed()
        self.assertEqual(self.handler.clients, 0)

    async def test_import_fails_explicitly_without_radio_backend(self):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, b'1-1'))
        await writer.drain()
        self.assertEqual(await reader.read(), bytes.fromhex('0111000300000001'))
        writer.close()
        await writer.wait_closed()

    async def test_recovery_import_preserves_rf_identity_without_new_association(self):
        backend=RadioBackend('020000000001',audio_bridge=True)
        backend.writer=object();backend.wcid=1;backend.generation=1
        await backend.recover_audio()
        self.assertFalse(backend.ready);self.assertTrue(backend.importable)
        self.handler.backend=backend
        reader,writer=await asyncio.open_connection('127.0.0.1',self.port)
        try:
            writer.write(struct.pack('!HHI32s',0x0111,0x8003,0,b'1-1'));await writer.drain()
            self.assertEqual((await asyncio.wait_for(reader.readexactly(320),1))[:8],bytes.fromhex('0111000300000000'))
            self.assertTrue(backend.ready);self.assertFalse(backend.recovery_required)
            self.assertEqual((backend.wcid,backend.generation),(1,2))
        finally:
            writer.close();await writer.wait_closed()
            await self.handler.close_clients()

    async def test_every_usb_import_starts_identity_session_without_replaying_old_replies(self):
        backend=RadioBackend('020000000001')
        backend.writer=object();backend.wcid=1;backend.generation=1
        announce=bytes((2,32,7,28))+bytes(28)
        stale=bytes((3,32,9,4,1,2,3,4))
        backend.announce=announce;backend.queue.append(announce);self.handler.backend=backend
        for index in range(2):
            reader,writer=await asyncio.open_connection('127.0.0.1',self.port)
            try:
                writer.write(struct.pack('!HHI32s',0x0111,0x8003,0,b'1-1'));await writer.drain()
                self.assertEqual((await asyncio.wait_for(reader.readexactly(320),1))[:8],bytes.fromhex('0111000300000000'))
                self.assertEqual(backend.usb_imports,index+1)
                self.assertEqual(tuple(backend.queue),(announce,))
                backend.queue.popleft();backend.queue.append(stale)
            finally:
                writer.close();await writer.wait_closed();await self.handler.close_clients()

    async def test_invalid_protocol_is_closed(self):
        for version, operation, status in [(0, 0x8005, 0), (0x0111, 0x8005, 1), (0x0111, 0x8004, 0)]:
            reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
            writer.write(struct.pack('!HHI', version, operation, status))
            await writer.drain()
            self.assertEqual(await reader.read(), b'')
            writer.close()
            await writer.wait_closed()

    async def test_incomplete_request_times_out_and_frees_connection(self):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(b'\x01')
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.read(), 2), b'')
        writer.close()
        await writer.wait_closed()
        self.assertEqual(self.handler.clients, 0)

    async def test_diagnostic_import_serves_descriptor_without_audio_backend(self):
        self.handler.enumeration_probe = True
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, b'1-1'))
        await writer.drain()
        self.assertEqual(await reader.readexactly(320), struct.pack('!HHI', 0x0111, 3, 0)+self.profile.usbip_device())
        header = struct.pack('!IIIIIIiiii8s', 1, 1, 0x10001, 1, 0, 0, 18, 0, 0, 0, bytes.fromhex('8006000100001200'))
        writer.write(header)
        await writer.drain()
        reply = await reader.readexactly(48)
        self.assertEqual(struct.unpack_from('!i', reply, 20)[0], 0)
        self.assertEqual(await reader.readexactly(18), self.profile.device)
        writer.close()
        await writer.wait_closed()
        for _ in range(20):
            if not self.handler.imported:
                break
            await asyncio.sleep(.01)
        self.assertFalse(self.handler.imported)

    async def test_diagnostic_import_rejects_unknown_bus_and_second_import(self):
        self.handler.enumeration_probe = True
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, b'1-1'))
        await writer.drain()
        await reader.readexactly(320)
        for bus in (b'1-1', b'2-2'):
            other_reader, other_writer = await asyncio.open_connection('127.0.0.1', self.port)
            other_writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, bus))
            await other_writer.drain()
            self.assertEqual(await other_reader.read(), bytes.fromhex('0111000300000001'))
            other_writer.close()
            await other_writer.wait_closed()
        writer.close()
        await writer.wait_closed()

    async def test_diagnostic_import_rejects_unknown_bus_when_idle(self):
        self.handler.enumeration_probe = True
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, b'2-2'))
        await writer.drain()
        self.assertEqual(await reader.read(), bytes.fromhex('0111000300000001'))
        writer.close()
        await writer.wait_closed()

    async def test_diagnostic_interrupt_in_waits_and_can_be_unlinked(self):
        self.handler.enumeration_probe = True
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, b'1-1'))
        await writer.drain()
        await reader.readexactly(320)
        writer.write(struct.pack('!IIIIIIiiii8s', 1, 1, 0x10001, 0, 0, 0, 0, 0, -1, 0, bytes.fromhex('0009010000000000')))
        await writer.drain()
        await reader.readexactly(48)
        writer.write(struct.pack('!IIIIIIiiii8s', 1, 2, 0x10001, 1, 1, 0, 64, 0, -1, 6, bytes(8)))
        await writer.drain()
        with self.assertRaises(TimeoutError):
            await asyncio.wait_for(reader.readexactly(48), .05)
        writer.write(struct.pack('!IIIIII24x', 2, 3, 0x10001, 0, 0, 2))
        await writer.drain()
        reply = await asyncio.wait_for(reader.readexactly(48), 1)
        self.assertEqual(struct.unpack_from('!IIIIIi', reply), (4, 3, 0, 0, 0, -104))
        writer.close()
        await writer.wait_closed()


if __name__ == '__main__':
    unittest.main()

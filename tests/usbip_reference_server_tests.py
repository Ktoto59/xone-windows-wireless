import asyncio
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import struct
import sys
import threading
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_headset_profile import HeadsetProfile
from usbip_reference_server import DiagnosticLog, ReferenceServer, StatusPublisher, transport_profile, main


class LoggingTests(unittest.TestCase):
    def test_slow_log_sink_is_bounded_and_does_not_block_radio_events(self):
        started, release = threading.Event(), threading.Event()
        def sink(fields):
            started.set()
            release.wait(2)
        log = DiagnosticLog(verbose=True, write=sink)
        try:
            log.emit({'operation': 'radio_associated'})
            self.assertTrue(started.wait(1))
            for _ in range(10000):
                log.emit({'operation': 'radio_audio_in'})
            self.assertEqual(log.queue.qsize(), 128)
            self.assertFalse(release.is_set())
        finally:
            release.set()
            log.close()

    def test_normal_operation_does_not_log_audio_packets(self):
        written = []
        log = DiagnosticLog(write=written.append)
        log.emit({'operation': 'radio_audio_in'})
        log.emit({'operation': 'radio_gip_out'})
        log.emit({'operation': 'radio_removed'})
        log.close()
        self.assertEqual(written, [{'operation': 'radio_removed'}])



class LifetimeTests(unittest.IsolatedAsyncioTestCase):
    async def test_zero_duration_serves_requests_until_cancelled(self):
        args = SimpleNamespace(profile=ROOT / 'driver/profiles/headset-0b32-usb.json',
                               audio_bridge=False, audio_probe=False, radio_mac=None,
                               enumeration_probe=False, port=0, seconds=0, status_file=None)
        output = StringIO()
        with redirect_stdout(output):
            task = asyncio.create_task(main(args))
            writer = None
            try:
                for _ in range(200):
                    if output.getvalue() or task.done():
                        break
                    await asyncio.sleep(.01)
                self.assertFalse(task.done(), 'Unlimited server stopped during startup')
                port = int(output.getvalue().split('127.0.0.1:')[1].split(';')[0])
                reader, writer = await asyncio.open_connection('127.0.0.1', port)
                writer.write(bytes.fromhex('0111800500000000'))
                await writer.drain()
                expected = HeadsetProfile.load(args.profile).usbip_device_list()
                self.assertEqual(await asyncio.wait_for(reader.read(), 3), expected)
                self.assertFalse(task.done(), 'Unlimited server stopped after serving a request')
            finally:
                if writer:
                    writer.close()
                    await writer.wait_closed()
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task


class TransportTests(unittest.IsolatedAsyncioTestCase):
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

    async def test_blocked_status_disk_does_not_block_usb_requests(self):
        started, release = threading.Event(), threading.Event()
        publisher = StatusPublisher(None, lambda: {'Ready': True})
        def slow_disk(snapshot):
            started.set()
            release.wait(3)
        publisher.write = slow_disk
        writer = None
        try:
            async with publisher:
                self.assertTrue(await asyncio.to_thread(started.wait, 1))
                reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
                writer.write(bytes.fromhex('0111800500000000'))
                await writer.drain()
                self.assertEqual(await asyncio.wait_for(reader.read(), .5), self.profile.usbip_device_list())
                self.assertFalse(release.is_set(), 'USB request needed disk IO to complete')
        finally:
            release.set()
            if writer:
                writer.close()
                await writer.wait_closed()

    async def test_import_fails_explicitly_without_radio_backend(self):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        writer.write(struct.pack('!HHI32s', 0x0111, 0x8003, 0, b'1-1'))
        await writer.drain()
        self.assertEqual(await reader.read(), bytes.fromhex('0111000300000001'))
        writer.close()
        await writer.wait_closed()

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

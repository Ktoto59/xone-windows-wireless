import asyncio
from dataclasses import replace
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_headset_profile import HeadsetProfile
from usbip_urb import (Completion, IsoPacket, MAX_TRANSFER, ProtocolError,
                       Submit, UrbSession, pack_completion, read_request)


def submit(seq, direction=1, ep=0, length=18, setup=None, data=b'', iso=(), count=None):
    if setup is None:
        setup = struct.pack('<BBHHH', 0x80, 6, 0x0100, 0, length) if ep == 0 else bytes(8)
    header = struct.pack('!IIIIIIiiii8s', 1, seq, 0x10001, direction, ep, 0,
                         length, -1, len(iso) if count is None else count, 4, setup)
    return header + data + b''.join(p.pack() for p in iso)


class CodecTests(unittest.IsolatedAsyncioTestCase):
    async def decode(self, data):
        reader = asyncio.StreamReader()
        reader.feed_data(data)
        reader.feed_eof()
        return await read_request(reader, timeout=.1)

    async def test_signed_fields_and_non_iso_minus_one(self):
        request = await self.decode(submit(17, count=-1))
        packet = pack_completion(request, Completion(data=b'abc', actual=3))
        self.assertEqual(len(packet), 51)
        self.assertEqual(struct.unpack_from('!IIIIIiiiii8x', packet), (3, 17, 0, 0, 0, 0, 3, 0, -1, 0))
        self.assertEqual(packet[48:], b'abc')

    async def test_native_windows_unlink_with_copied_padding(self):
        packet = bytes.fromhex('000000020000001c0001000100000000000000000000001a0000000000000000ffffffff000000000009010000000000')
        request = await self.decode(packet)
        self.assertEqual((request.seq, request.target), (28, 26))

    async def test_invalid_headers_rejected_before_payload_allocation(self):
        valid = submit(1)
        for offset, value in [(0, 99), (8, 99), (12, 2), (16, 16), (24, MAX_TRANSFER+1), (32, 257)]:
            packet = bytearray(valid)
            struct.pack_into('!I', packet, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ProtocolError):
                await self.decode(packet)

    async def test_control_length_and_direction_validated(self):
        for direction, length in [(0, 18), (1, 17)]:
            with self.assertRaises(ProtocolError):
                await self.decode(submit(1, direction=direction, length=length,
                                         setup=bytes.fromhex('8006000100001200')))

    async def test_iso_offsets_and_compacted_in_completion(self):
        request = await self.decode(submit(1, ep=2, length=16,
                                          iso=(IsoPacket(0, 6), IsoPacket(8, 6))))
        result = Completion(data=b'abcde', actual=5,
                            iso=(IsoPacket(0, 6, 2), IsoPacket(8, 6, 3)), frame=123)
        packet = pack_completion(request, result)
        self.assertEqual(packet[48:53], b'abcde')
        self.assertEqual(packet[53:], result.iso[0].pack()+result.iso[1].pack())
        self.assertEqual(struct.unpack_from('!ii', packet, 28), (123, 2))
        with self.assertRaises(ProtocolError):
            pack_completion(request, replace(result, actual=6, data=b'abcdef'))

    async def test_iso_overlap_outside_buffer_and_request_result_fields(self):
        for packets in [(IsoPacket(0, 10), IsoPacket(8, 2)),
                        (IsoPacket(14, 3),), (IsoPacket(0, 2, 1),)]:
            with self.subTest(packets=packets), self.assertRaises(ProtocolError):
                await self.decode(submit(1, ep=2, length=16, iso=packets))

    async def test_out_payload_is_consumed_before_next_header(self):
        reader = asyncio.StreamReader()
        reader.feed_data(submit(1, direction=0, ep=1, length=4, data=b'abcd')+submit(2))
        first, second = await read_request(reader), await read_request(reader)
        self.assertEqual(first.data, b'abcd')
        self.assertEqual(second.seq, 2)

    async def test_completion_does_not_claim_excess_data(self):
        request = await self.decode(submit(1))
        for result in [Completion(actual=19, data=b'a'*19), Completion(actual=2, data=b'a')]:
            with self.assertRaises(ProtocolError):
                pack_completion(request, result)


class PendingBackend:
    def __init__(self):
        self.entered = asyncio.Event()
        self.cancelled = asyncio.Event()
        self.release = asyncio.Event()

    async def submit(self, request, endpoint):
        self.entered.set()
        try:
            await self.release.wait()
            return Completion(actual=1, data=b'A')
        except asyncio.CancelledError:
            self.cancelled.set()
            raise


class SessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.profile = HeadsetProfile.load(ROOT / 'driver/profiles/headset-0b32-usb.json')
        self.backend = PendingBackend()
        self.sessions = []
        self.events = []
        async def connection(reader, writer):
            session = UrbSession(self.profile, self.backend, timeout=.5, on_event=self.events.append)
            self.sessions.append(session)
            await session.run(reader, writer)
        self.server = await asyncio.start_server(connection, '127.0.0.1', 0)
        self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.server.sockets[0].getsockname()[1])

    async def asyncTearDown(self):
        self.writer.close()
        await self.writer.wait_closed()
        self.server.close()
        await self.server.wait_closed()
        for _ in range(20):
            if all(not s.pending for s in self.sessions):
                break
            await asyncio.sleep(.01)
        self.assertTrue(all(not s.pending for s in self.sessions))

    async def receive(self, timeout=1):
        header = await asyncio.wait_for(self.reader.readexactly(48), timeout)
        values = struct.unpack('!IIIIIiiiii8s', header)
        if values[0] == 3:
            # Tests only request IN or zero-length OUT controls.
            data = await self.reader.readexactly(values[6])
            iso = await self.reader.readexactly(max(0, values[8])*16)
        else:
            data, iso = b'', b''
        return values, data, iso

    async def configure(self, seq=1):
        self.writer.write(submit(seq, direction=0, length=0, setup=bytes.fromhex('0009010000000000')))
        await self.writer.drain()
        values, _, _ = await self.receive()
        self.assertEqual(values[5], 0)

    async def pending(self, seq=2):
        await self.configure()
        self.writer.write(submit(seq, ep=1, length=64))
        await self.writer.drain()
        await asyncio.wait_for(self.backend.entered.wait(), 1)

    async def test_fragmented_control_request_and_vendor_stall(self):
        for value in submit(1):
            self.writer.write(bytes((value,)))
            await self.writer.drain()
            await asyncio.sleep(0)
        values, data, _ = await self.receive()
        self.assertEqual(data, self.profile.device)
        self.assertEqual(values[5], 0)
        self.writer.write(submit(2, length=4, setup=bytes.fromhex('c0ff000000000400')))
        values, data, _ = await self.receive()
        self.assertEqual((values[5], data), (-32, b''))

    async def test_disabled_endpoint_never_reaches_backend(self):
        self.writer.write(submit(1, ep=2, length=64, iso=(IsoPacket(0, 64),)))
        values, _, iso = await self.receive()
        self.assertEqual(values[5], -32)
        self.assertEqual(struct.unpack('!IIIi', iso), (0, 64, 0, -32))
        self.assertFalse(self.backend.entered.is_set())

    async def test_native_gip_iso_slot_is_buffer_capacity_not_max_packet(self):
        await self.configure()
        self.writer.write(submit(2, direction=0, length=0, setup=bytes.fromhex('010b010001000000')))
        values, _, _ = await self.receive()
        self.assertEqual(values[5], 0)
        self.writer.write(submit(3, ep=2, length=4096, iso=(IsoPacket(0, 4096),)))
        await self.writer.drain()
        await asyncio.wait_for(self.backend.entered.wait(), 1)
        self.writer.write(struct.pack('!IIIIII24x', 2, 4, 0x10001, 0, 0, 3))
        values, _, _ = await self.receive()
        self.assertEqual(values[:6], (4, 4, 0, 0, 0, -104))
        await asyncio.wait_for(self.backend.cancelled.wait(), 1)

    async def test_unlink_cancels_pending_without_ret_submit(self):
        await self.pending()
        self.writer.write(struct.pack('!IIIIII24x', 2, 3, 0x10001, 0, 0, 2))
        values, _, _ = await self.receive()
        self.assertEqual(values[:6], (4, 3, 0, 0, 0, -104))
        await asyncio.wait_for(self.backend.cancelled.wait(), 1)
        # Next packet must be this control response, never seq 2 RET_SUBMIT.
        self.writer.write(submit(4))
        values, _, _ = await self.receive()
        self.assertEqual(values[1], 4)

    async def test_unlink_after_completion_returns_zero(self):
        await self.pending()
        self.backend.release.set()
        values, data, _ = await self.receive()
        self.assertEqual((values[1], data), (2, b'A'))
        self.writer.write(struct.pack('!IIIIII24x', 2, 3, 0x10001, 0, 0, 2))
        values, _, _ = await self.receive()
        self.assertEqual(values[5], 0)

    async def test_unconfigure_flushes_pending_with_shutdown(self):
        await self.pending()
        self.writer.write(submit(3, direction=0, length=0, setup=bytes.fromhex('0009000000000000')))
        control, _, _ = await self.receive()
        flushed, _, _ = await self.receive()
        self.assertEqual((control[1], control[5], flushed[1], flushed[5]), (3, 0, 2, -108))
        await asyncio.wait_for(self.backend.cancelled.wait(), 1)

    async def test_duplicate_pending_seq_closes_connection(self):
        await self.pending()
        self.writer.write(submit(2, ep=1, length=64))
        self.assertEqual(await asyncio.wait_for(self.reader.read(), 1), b'')
        await asyncio.wait_for(self.backend.cancelled.wait(), 1)

    async def test_disconnect_cancels_backend(self):
        await self.pending()
        self.writer.close()
        await self.writer.wait_closed()
        await asyncio.wait_for(self.backend.cancelled.wait(), 1)

    async def test_pending_limit_fails_without_allocating_another_task(self):
        await self.configure()
        for seq in range(2, 67):
            self.writer.write(submit(seq, ep=1, length=64))
        await self.writer.drain()
        # This is a resource-limit check after 65 sequential decodes/tasks,
        # not a one-second Windows scheduler performance requirement.
        values, _, _ = await self.receive(timeout=5)
        self.assertEqual((values[1], values[5]), (66, -12))
        self.assertEqual(len(self.sessions[0].pending), 64)


if __name__ == '__main__':
    unittest.main()

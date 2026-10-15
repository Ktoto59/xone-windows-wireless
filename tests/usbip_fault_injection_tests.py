"""Faults across real loopback RF IPC -> RadioBackend -> USB/IP wire.

No physical USB access. Controlled media clock keeps expiry tests reproducible;
explicit asyncio sleeps model actual delayed ACKs/byte-stream delivery as well.
"""
import asyncio
from pathlib import Path
import struct
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_gip_radio import RadioBackend, frame, read_frame, gip_packets, AUDIO_MAX_AGE
from usbip_reference_server import transport_profile
from usbip_urb import IsoPacket, UrbSession

MAC = '020000000101'
COMMAND = bytes.fromhex('04200100')


class FaultInjectionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.now = 100.0
        self.backend = RadioBackend(MAC, audio_bridge=True, clock=lambda: self.now)
        self.radio_server = await asyncio.start_server(self.backend.connection, '127.0.0.1', 0)
        self.radio_reader, self.radio_writer = await asyncio.open_connection('127.0.0.1', self.radio_server.sockets[0].getsockname()[1])
        self.radio_writer.write(frame(0,0,bytes.fromhex(MAC))+frame(1,1,bytes.fromhex(MAC)))
        await self.radio_writer.drain()
        await self.until(lambda: self.backend.ready)
        self.sessions = []
        self.profile = transport_profile(ROOT / 'driver/profiles/headset-0b32-usb.json', True)
        self.usb_server = await asyncio.start_server(self.usb_connection, '127.0.0.1', 0)
        self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.usb_server.sockets[0].getsockname()[1])
        self.sequence, self.directions, self.results = 0, {}, {}
        for setup in ('0009010000000000', '010b010001000000'):
            seq = self.submit(direction=0, ep=0, setup=bytes.fromhex(setup))
            self.assertEqual((await self.receive(seq))['status'],0)

    async def usb_connection(self, reader, writer):
        session = UrbSession(self.profile, backend=self.backend, timeout=5)
        self.sessions.append(session)
        await session.run(reader,writer)

    async def asyncTearDown(self):
        self.writer.close();self.radio_writer.close()
        await asyncio.gather(self.writer.wait_closed(),self.radio_writer.wait_closed())
        self.usb_server.close();self.radio_server.close()
        await asyncio.gather(self.usb_server.wait_closed(),self.radio_server.wait_closed())
        await self.until(lambda: not self.backend.ready and all(not s.pending for s in self.sessions))

    async def until(self, predicate):
        async def poll():
            while not predicate():
                await asyncio.sleep(0)
        await asyncio.wait_for(poll(),3)

    def submit(self, *, direction=1, ep=1, length=0, data=b'', iso=(), setup=bytes(8)):
        self.sequence += 1
        self.directions[self.sequence] = direction
        header = struct.pack('!IIIIIIiiii8s',1,self.sequence,0x10001,direction,ep,0,length,-1,len(iso),1,setup)
        self.writer.write(header+data+b''.join(p.pack() for p in iso))
        return self.sequence

    async def receive(self, seq):
        while seq not in self.results:
            h = await asyncio.wait_for(self.reader.readexactly(48),3)
            command, number, _, _, _, status, actual, _, count, errors = struct.unpack('!IIIIIiiiii8x',h)
            self.assertEqual(command,3)
            data = await self.reader.readexactly(actual) if self.directions[number] and actual else b''
            raw = await self.reader.readexactly(max(count,0)*16)
            self.results[number] = dict(status=status,actual=actual,data=data,errors=errors,
                                        iso=tuple(IsoPacket(*v) for v in struct.iter_unpack('!IIIi',raw)))
        return self.results.pop(seq)

    async def feed_audio(self, value):
        before = self.backend.audio_rx
        self.radio_writer.write(frame(4,1,bytes.fromhex('6020018283000006')+bytes([value])*384))
        await self.radio_writer.drain()
        await self.until(lambda: self.backend.audio_rx > before)

    def capture(self):
        return self.submit(ep=2,length=512,iso=tuple(IsoPacket(i*64,64) for i in range(8)))

    def render(self, value=22):
        packet = bytes.fromhex('602001c08100')+bytes([value])*192
        return self.submit(direction=0,ep=2,length=198*8,data=packet*8,
                           iso=tuple(IsoPacket(i*198,198) for i in range(8)))

    async def ack(self, kind):
        actual,wcid,token,payload = await read_frame(self.radio_reader)
        self.assertEqual(actual,kind)
        self.radio_writer.write(frame(17,wcid,struct.pack('<i',0),token))
        await self.radio_writer.drain()
        return payload

    async def control_roundtrip(self):
        seq = self.submit(direction=0,length=4,data=COMMAND)
        self.assertEqual(await self.ack(16),COMMAND)
        result = await self.receive(seq)
        self.assertEqual((result['status'],result['actual']),(0,4))

    async def test_fault_scheduler_pause_sweep_20_to_500_ms(self):
        for milliseconds in (20,50,100,250,500):
            with self.subTest(pause_ms=milliseconds):
                await self.feed_audio(11)
                self.now += milliseconds/1000
                seq = self.capture()
                if milliseconds > AUDIO_MAX_AGE*1000:
                    await self.until(lambda: self.backend.capture_dropped > 0)
                    await self.feed_audio(22)
                result = await self.receive(seq)
                expected = 11 if milliseconds <= AUDIO_MAX_AGE*1000 else 22
                self.assertEqual(b''.join(p[6:] for p in gip_packets(result['data'])),bytes([expected])*384)
                self.assertEqual(result['errors'],0)
                await self.control_roundtrip()
                self.assertEqual(self.backend.generation,1)
                self.assertTrue(self.backend.ready)

    async def test_fault_delayed_audio_ack_keeps_commands_live_and_discards_tail(self):
        await self.feed_audio(1)
        first = self.render(11)
        kind,wcid,token,payload = await read_frame(self.radio_reader)
        self.assertEqual((kind,payload[6:]),(18,bytes([11])*1536))
        queued = [self.render(33) for _ in range(8)]
        await self.until(lambda: len(self.sessions[0].pending) == 9)
        # An actual delayed ACK, while the event loop must serve control traffic.
        await asyncio.sleep(.05)
        await self.control_roundtrip()
        self.now += .25
        self.radio_writer.write(frame(17,wcid,struct.pack('<i',0),token));await self.radio_writer.drain()
        self.assertEqual((await self.receive(first))['actual'],1584)
        for seq in queued:
            result = await self.receive(seq)
            self.assertEqual((result['actual'],result['errors']),(0,8))
            self.assertTrue(all(p.status == -18 for p in result['iso']))
        self.assertEqual(self.backend.render_dropped,8)
        await self.feed_audio(2)
        fresh = self.render(44)
        self.assertEqual((await self.ack(18))[6:],bytes([44])*1536)
        self.assertEqual((await self.receive(fresh))['errors'],0)
        self.assertTrue(self.backend.ready)
        self.assertEqual(self.backend.generation,1)

    async def test_fault_audio_packet_loss_and_jitter_resume_without_replug(self):
        intervals = (.008,.011,.004,.020,.050,.005,.100,.008,.500,.008)
        for i, interval in enumerate(intervals):
            self.now += interval
            seq = self.capture()
            if i % 3 == 1:
                # Omit a whole RF audio message (not arbitrary TCP bytes).
                await self.until(lambda: seq in self.sessions[0].pending)
                await self.control_roundtrip()
                self.now += .040
            await self.feed_audio(50+i)
            result = await self.receive(seq)
            self.assertEqual(result['errors'],0)
            self.assertEqual(b''.join(p[6:] for p in gip_packets(result['data'])),bytes([50+i])*384)
            self.assertFalse(self.backend.audio_queue)
        self.assertTrue(self.backend.ready)
        self.assertEqual(self.backend.generation,1)

    async def test_fault_fragmented_tcp_with_actual_delivery_delays(self):
        seq = self.submit(length=64)
        packet = frame(2,1,COMMAND)
        # Delay the stream, preserving ordering and all bytes, as TCP does.
        for at, delay in ((0,.020),(5,.050),(11,.100)):
            end = 5 if at == 0 else 11 if at == 5 else len(packet)
            self.radio_writer.write(packet[at:end]);await self.radio_writer.drain()
            await asyncio.sleep(delay)
        result = await self.receive(seq)
        self.assertEqual((result['status'],result['data']),(0,COMMAND))
        await self.feed_audio(77)
        capture = await self.receive(self.capture())
        self.assertEqual(capture['actual'],432)
        self.assertEqual(self.backend.generation,1)

    async def test_fault_actual_event_loop_stall_discards_old_capture_and_recovers(self):
        self.backend.clock = time.perf_counter
        await self.feed_audio(11)
        # Deliberately block this loop: mirrors a scheduler/accidental blocking
        # callback pause, rather than just advancing the controlled clock.
        time.sleep(.100)
        seq = self.capture()
        await self.until(lambda: self.backend.capture_dropped == 8)
        await self.control_roundtrip()
        await self.feed_audio(99)
        result = await self.receive(seq)
        self.assertEqual(b''.join(p[6:] for p in gip_packets(result['data'])),bytes([99])*384)
        self.assertEqual(result['errors'],0)
        self.assertEqual(self.backend.generation,1)
        self.assertTrue(self.backend.ready)


if __name__ == '__main__':
    unittest.main()

import asyncio
from dataclasses import replace
from pathlib import Path
import struct
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_gip_radio import RadioBackend, frame, read_frame, gip_packets, usb_interrupt_packets, encode_chunk, gip_header_size
from usb_headset_profile import Endpoint
from usbip_urb import Submit, IsoPacket

MAC = '020000000001'


def request(incoming=True, length=64, data=b''):
    return Submit(1, int(incoming), 1, 0, length, 0, -1, 4, bytes(8), data, ())


class FramingTests(unittest.IsolatedAsyncioTestCase):
    async def test_rf_chunks_split_with_original_ack_byte_counts_and_sequence(self):
        payload = bytes(range(250))*2
        packets = [encode_chunk(4,240,149,payload[:122],500),
                   encode_chunk(4,160,149,payload[122:244],122),
                   encode_chunk(4,176,149,payload[244:],244),
                   encode_chunk(4,160,149,b'',500)]
        result = usb_interrupt_packets(b''.join(packets))
        reconstructed = bytearray(500)
        acknowledgements = []
        seen = bytearray(500)
        for packet in result:
            self.assertLessEqual(len(packet),64)
            self.assertEqual((packet[0],packet[2]),(4,149))
            at = 3
            def integer():
                nonlocal at
                value,shift = 0,0
                while True:
                    part=packet[at];at+=1;value|=(part & 127) << shift;shift+=7
                    if not part & 128:return value
            length,offset = integer(),integer()
            if packet[1] & 64:
                self.assertEqual(offset,500)
                offset=0
            reconstructed[offset:offset+length]=packet[at:]
            seen[offset:offset+length]=b'\1'*length
            if packet[1] & 16:acknowledgements.append(offset+length)
        self.assertEqual(reconstructed,payload)
        self.assertTrue(all(seen))
        self.assertEqual(acknowledgements,[122,500])
        self.assertEqual(result[-1],packets[-1])
        with self.assertRaises(ValueError):
            usb_interrupt_packets(bytes((4,32,1,100))+bytes(100))

    async def test_fragmented_ipc_and_coalesced_packets(self):
        reader = asyncio.StreamReader()
        packet = frame(2, 1, b'\x04\x20\x01\x00', 0x12345678)
        async def feed():
            for byte in packet:
                reader.feed_data(bytes((byte,)))
                await asyncio.sleep(0)
        task = asyncio.create_task(feed())
        self.assertEqual(await read_frame(reader), (2, 1, 0x12345678, b'\x04\x20\x01\x00'))
        await task
        self.assertEqual(gip_packets(bytes.fromhex('0420010001200103000400')), [bytes.fromhex('04200100'), bytes.fromhex('01200103000400')])

    async def test_invalid_ipc_and_truncated_gip_rejected(self):
        for header in [struct.pack('<4sBBHI', b'BAD!', 2, 1, 0, 0), struct.pack('<4sBBHI', b'XGP1', 2, 1, 1601, 0)]:
            reader = asyncio.StreamReader();reader.feed_data(header)
            with self.assertRaises(ValueError):
                await read_frame(reader)
        for data in [b'', b'\x04', bytes.fromhex('0420010500'), bytes.fromhex('04200180808080')]:
            with self.assertRaises(ValueError):
                gip_packets(data)


class BackendTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.backend = RadioBackend(MAC)
        self.server = await asyncio.start_server(self.backend.connection, '127.0.0.1', 0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.port)
        self.writer.write(frame(0, 0, bytes.fromhex(MAC))+frame(1, 1, bytes.fromhex(MAC)))
        await self.writer.drain()
        for _ in range(50):
            if self.backend.ready:
                break
            await asyncio.sleep(.001)
        self.assertTrue(self.backend.ready)

    async def asyncTearDown(self):
        self.writer.close()
        await self.writer.wait_closed()
        self.server.close()
        await self.server.wait_closed()
        for _ in range(50):
            if not self.backend.writer:
                break
            await asyncio.sleep(.001)
        self.assertFalse(self.backend.ready)
        self.assertFalse(self.backend.pending)

    async def test_interrupt_input_preserves_exact_gip_bytes(self):
        pending = asyncio.create_task(self.backend.submit(request(), Endpoint(0x81, 3, 64, 4)))
        # Use an actual valid chunk header/payload shape, unchanged end to end.
        data = bytes.fromhex('04e00782000a0102')
        self.writer.write(frame(2, 1, data))
        await self.writer.drain()
        result = await asyncio.wait_for(pending, 1)
        self.assertEqual((result.status, result.data, result.actual), (0, data, len(data)))

    async def test_output_waits_for_radio_ack_and_propagates_error(self):
        for status in (0, -71):
            data = bytes.fromhex('04200100')
            pending = asyncio.create_task(self.backend.submit(request(False, len(data), data), Endpoint(1, 3, 64, 4)))
            kind, wcid, token, payload = await read_frame(self.reader)
            self.assertEqual((kind, wcid, payload), (16, 1, data))
            self.assertFalse(pending.done())
            self.writer.write(frame(17, wcid, struct.pack('<i', status), token))
            await self.writer.drain()
            result = await pending
            self.assertEqual((result.status, result.actual), (status, len(data) if status == 0 else 0))

    async def test_large_radio_identification_delivered_in_usb_sized_chunks(self):
        data = encode_chunk(4,240,149,bytes(range(122)),500)
        self.assertEqual(len(data),128)
        self.writer.write(frame(2,1,data));await self.writer.drain()
        observed = []
        for _ in range(3):
            result=await asyncio.wait_for(self.backend.submit(request(),Endpoint(0x81,3,64,4)),1)
            self.assertEqual(result.status,0)
            observed.append(result.data)
        self.assertEqual(b''.join(p[gip_header_size(p):] for p in observed),bytes(range(122)))
        self.assertEqual([bool(p[1]&16) for p in observed],[False,False,True])
        # The USB ACK's full RF chunk byte count goes unchanged to the radio.
        ack=bytes.fromhex('012095090004207a0000007a01')
        task=asyncio.create_task(self.backend.submit(request(False,len(ack),ack),Endpoint(1,3,64,4)))
        kind,wcid,token,payload=await read_frame(self.reader)
        self.assertEqual((kind,wcid,payload),(16,1,ack))
        self.writer.write(frame(17,1,struct.pack('<i',0),token));await self.writer.drain()
        self.assertEqual((await task).status,0)

    async def test_battery_observer_preserves_status_and_ignores_accessory_and_fragment(self):
        parent = bytes.fromhex('032002048a000000')
        self.writer.write(frame(2,1,parent));await self.writer.drain()
        result = await self.backend.submit(request(),Endpoint(0x81,3,64,4))
        self.assertEqual(result.data,parent)
        self.assertEqual(self.backend.status()['RadioBattery'],
                         {'Connected':True,'Type':'play-and-charge','Level':'high','RawType':2,'RawLevel':2})
        for data in (bytes.fromhex('0321030487000000'),bytes.fromhex('03a004040487000000'),bytes.fromhex('0320050187')):
            self.writer.write(frame(2,1,data));await self.writer.drain()
            result = await self.backend.submit(request(),Endpoint(0x81,3,64,4))
            self.assertEqual(result.data,data)
            self.assertEqual(self.backend.battery['Level'],'high')
        await self.backend.reset()
        self.assertIsNone(self.backend.status()['RadioBattery'])

    async def test_unlinked_input_does_not_consume_later_packet(self):
        task = asyncio.create_task(self.backend.submit(request(), Endpoint(0x81, 3, 64, 4)))
        await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        data = bytes.fromhex('0320020480000000')
        self.writer.write(frame(2, 1, data));await self.writer.drain()
        result = await self.backend.submit(request(), Endpoint(0x81, 3, 64, 4))
        self.assertEqual(result.data, data)

    async def test_radio_removal_fails_pending_input(self):
        task = asyncio.create_task(self.backend.submit(request(), Endpoint(0x81, 3, 64, 4)))
        await asyncio.sleep(0)
        self.writer.write(frame(3, 1));await self.writer.drain()
        self.assertEqual((await task).status, -19)

    async def test_disconnect_and_reassociation_cannot_complete_old_write(self):
        data = bytes.fromhex('04200100')
        task = asyncio.create_task(self.backend.submit(request(False, len(data), data), Endpoint(1, 3, 64, 4)))
        await read_frame(self.reader)
        self.writer.write(frame(1, 1, bytes.fromhex(MAC)));await self.writer.drain()
        self.assertEqual((await task).status, -19)

    async def test_cancelled_write_releases_completion_token(self):
        data = bytes.fromhex('04200100')
        task = asyncio.create_task(self.backend.submit(request(False, len(data), data), Endpoint(1, 3, 64, 4)))
        await read_frame(self.reader)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertFalse(self.backend.pending)

    async def test_short_input_does_not_discard_packet(self):
        data = bytes.fromhex('0320020480000000')
        self.writer.write(frame(2, 1, data));await self.writer.drain()
        result = await self.backend.submit(request(length=4), Endpoint(0x81, 3, 64, 4))
        self.assertEqual(result.status, -90)
        result = await self.backend.submit(request(), Endpoint(0x81, 3, 64, 4))
        self.assertEqual(result.data, data)

    async def test_iso_audio_never_acknowledged_by_interrupt_broker(self):
        result = await self.backend.submit(request(), Endpoint(0x82, 1, 64, 1))
        self.assertEqual(result.status, -32)
        data = bytes.fromhex('60200100')
        result = await self.backend.submit(request(False, len(data), data), Endpoint(1, 3, 64, 4))
        self.assertEqual(result.status, -95)

    async def test_second_radio_connection_cannot_replace_active_one(self):
        other_reader, other_writer = await asyncio.open_connection('127.0.0.1', self.port)
        self.assertEqual(await other_reader.read(), b'')
        self.assertTrue(self.backend.ready)
        other_writer.close();await other_writer.wait_closed()

    async def test_capture_probe_preserves_radio_frame_and_large_native_slot(self):
        self.backend.audio_probe = True
        capture = replace(request(), ep=2, length=4096, packet_count=1, iso=(IsoPacket(0, 4096),))
        task = asyncio.create_task(self.backend.submit(capture, Endpoint(0x82, 1, 64, 1)))
        packet = bytes.fromhex('60200162')+b'\0\0'+bytes(range(96))
        self.writer.write(frame(4, 1, packet));await self.writer.drain()
        result = await asyncio.wait_for(task, 1)
        self.assertEqual((result.status, result.data, result.actual), (0, packet, len(packet)))
        self.assertEqual(result.iso, (IsoPacket(0, 4096, len(packet)),))
        self.assertFalse(self.backend.queue)

    async def test_cancelled_capture_probe_does_not_consume_next_audio(self):
        self.backend.audio_probe = True
        capture = replace(request(), ep=2, length=64, packet_count=1, iso=(IsoPacket(0, 64),))
        task = asyncio.create_task(self.backend.submit(capture, Endpoint(0x82, 1, 64, 1)))
        await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        packet = bytes.fromhex('602001020000')
        self.writer.write(frame(4, 1, packet));await self.writer.drain()
        result = await self.backend.submit(capture, Endpoint(0x82, 1, 64, 1))
        self.assertEqual(result.data, packet)

    async def test_render_probe_logs_only_headers_and_fails_without_radio_write(self):
        self.backend.audio_probe = True
        events = []
        self.backend.on_event = events.append
        packet = bytes.fromhex('6020010401020304')
        render = replace(request(False, len(packet), packet), ep=2, packet_count=1, iso=(IsoPacket(0, len(packet)),))
        result = await self.backend.submit(render, Endpoint(2, 1, 228, 1))
        self.assertEqual(result.status, -95)
        self.assertEqual(events[0]['headers'], [{'command':96,'options':32,'sequence':1,'length':8}])
        self.assertNotIn('01020304', str(events))
        self.assertFalse(self.backend.pending)

    async def test_render_bridge_groups_eight_usb_fragments_without_pcm_changes(self):
        self.backend.audio_probe = self.backend.audio_bridge = self.backend.audio_layout = True
        self.backend.render_credits = 1
        pieces = [bytes([i])*192 for i in range(8)]
        def render(pcm):
            packet = bytes.fromhex('602001c08100')+pcm
            return replace(request(False,len(packet),packet),ep=2,packet_count=1,iso=(IsoPacket(0,len(packet)),))
        for pcm in pieces[:7]:
            result = await self.backend.submit(render(pcm),Endpoint(2,1,228,1))
            self.assertEqual(result.status,0)
        task = asyncio.create_task(self.backend.submit(render(pieces[-1]),Endpoint(2,1,228,1)))
        kind,wcid,token,data = await read_frame(self.reader)
        self.assertEqual((kind,wcid),(18,1))
        self.assertEqual(data,bytes.fromhex('602001808c00')+b''.join(pieces))
        self.assertFalse(task.done())
        self.writer.write(frame(17,1,struct.pack('<i',0),token));await self.writer.drain()
        self.assertEqual((await task).actual,198)
        self.assertFalse(self.backend.render_pcm)
        self.assertEqual((self.backend.render_nonzero,self.backend.render_peak),(1,0x0707))

    async def test_render_bridge_rejects_unobserved_format_and_clears_partial_on_loss(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        packet = bytes.fromhex('602001c08100')+bytes(192)
        render = replace(request(False,len(packet),packet),ep=2,packet_count=1,iso=(IsoPacket(0,len(packet)),))
        self.assertEqual((await self.backend.submit(render,Endpoint(2,1,228,1))).status,-95)
        self.backend.audio_layout = True
        self.assertEqual((await self.backend.submit(render,Endpoint(2,1,228,1))).status,0)
        self.assertEqual(len(self.backend.render_pcm),192)
        await self.backend.reset()
        self.assertFalse(self.backend.render_pcm)
        self.assertFalse(self.backend.audio_layout)

    async def test_late_audio_completion_does_not_reset_reassociated_radio(self):
        self.backend.audio_probe = self.backend.audio_bridge = self.backend.audio_layout = True
        self.backend.render_credits = 1
        packet = bytes.fromhex('602001c08100')+bytes(192)
        render = replace(request(False,len(packet),packet),ep=2,packet_count=1,iso=(IsoPacket(0,len(packet)),))
        for _ in range(7):
            await self.backend.submit(render,Endpoint(2,1,228,1))
        task = asyncio.create_task(self.backend.submit(render,Endpoint(2,1,228,1)))
        await read_frame(self.reader)
        self.writer.write(frame(1,1,bytes.fromhex(MAC)));await self.writer.drain()
        self.assertEqual((await task).status,-19)
        self.assertTrue(self.backend.ready)

    async def test_capture_bridge_emits_eight_one_ms_frames_and_preserves_pcm(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        pcm = bytes(range(256))+bytes(range(128))
        radio = bytes.fromhex('6020018283000006')+pcm
        capture = replace(request(),ep=2,length=4096,packet_count=1,iso=(IsoPacket(0,4096),))
        task = asyncio.create_task(self.backend.submit(capture,Endpoint(0x82,1,64,1)))
        self.writer.write(frame(4,1,radio));await self.writer.drain()
        result = await task
        results = [result]
        for _ in range(7):
            results.append(await self.backend.submit(capture,Endpoint(0x82,1,64,1)))
        self.assertTrue(all(len(gip_packets(r.data)) == 1 for r in results))
        self.assertTrue(all(r.actual == 54 and r.iso[0].actual == 54 for r in results))
        packets = [r.data for r in results]
        self.assertEqual(len(packets),8)
        self.assertEqual([p[2] for p in packets],list(range(1,9)))
        self.assertTrue(all(len(p)==54 and p[4:6]==bytes((192,0)) for p in packets))
        self.assertEqual(b''.join(p[6:] for p in packets),pcm)
        self.assertFalse(self.backend.audio_queue)

    async def test_cancelled_paced_capture_preserves_pcm_for_next_request(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        self.backend.audio_queue.append(bytes.fromhex('60200132c000')+bytes(48))
        self.backend.capture_deadline = time.perf_counter()+.03
        capture = replace(request(),ep=2,length=4096,packet_count=1,iso=(IsoPacket(0,4096),))
        task = asyncio.create_task(self.backend.submit(capture,Endpoint(0x82,1,64,1)))
        await asyncio.sleep(.005)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(len(self.backend.audio_queue),1)
        self.backend.capture_deadline = 0
        self.assertEqual((await self.backend.submit(capture,Endpoint(0x82,1,64,1))).actual,54)

    async def test_eight_slot_capture_packs_pcm_and_keeps_iso_offsets(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        pcm = bytes(range(256))+bytes(range(128))
        radio = bytes.fromhex('6020018283000006')+pcm
        slots = tuple(IsoPacket(i*64,64) for i in range(8))
        capture = replace(request(),ep=2,length=512,packet_count=8,iso=slots)
        task = asyncio.create_task(self.backend.submit(capture,Endpoint(0x82,1,64,1)))
        self.writer.write(frame(4,1,radio));await self.writer.drain()
        result = await task
        self.assertEqual(result.actual,432)
        self.assertEqual([p.offset for p in result.iso],[i*64 for i in range(8)])
        self.assertTrue(all(p.actual == 54 for p in result.iso))
        packets = gip_packets(result.data)
        self.assertEqual(b''.join(p[6:] for p in packets),pcm)
        self.assertFalse(self.backend.audio_queue)

    async def test_eight_slot_capture_uses_radio_clock_not_stale_host_deadline(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        self.backend.capture_deadline = time.perf_counter()+10
        radio = bytes.fromhex('6020018283000006')+bytes(384)
        capture = replace(request(),ep=2,length=512,packet_count=8,
                          iso=tuple(IsoPacket(i*64,64) for i in range(8)))
        task = asyncio.create_task(self.backend.submit(capture,Endpoint(0x82,1,64,1)))
        self.writer.write(frame(4,1,radio));await self.writer.drain()
        result = await asyncio.wait_for(task,1)
        self.assertEqual((result.status,result.actual),(0,432))
        self.assertEqual(self.backend.capture_deadline,0)

    async def test_capture_overrun_keeps_recent_pcm_and_interrupt_connection_alive(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        for index in range(300):
            radio = bytes.fromhex('6020018283000006')+bytes((index%256,))*384
            self.writer.write(frame(4,1,radio))
        command = bytes.fromhex('0320020480000000')
        self.writer.write(frame(2,1,command));await self.writer.drain()
        interrupt = await asyncio.wait_for(self.backend.submit(request(),Endpoint(0x81,3,64,4)),10)
        self.assertEqual(interrupt.data,command)
        self.assertTrue(self.backend.ready)
        self.assertEqual(self.backend.generation,1)
        self.assertEqual(len(self.backend.audio_queue),64)
        self.assertEqual(self.backend.capture_dropped,300*8-64)
        self.assertEqual(self.backend.capture_high_water,64)
        capture = replace(request(),ep=2,length=512,packet_count=8,
                          iso=tuple(IsoPacket(i*64,64) for i in range(8)))
        result = await self.backend.submit(capture,Endpoint(0x82,1,64,1))
        self.assertEqual(result.status,0)
        self.assertEqual(b''.join(p[6:] for p in gip_packets(result.data)),bytes((292%256,))*384)

    async def test_oversized_capture_request_fails_without_waiting_for_unbounded_audio(self):
        self.backend.audio_probe = self.backend.audio_bridge = True
        capture = replace(request(),ep=2,length=65*64,packet_count=65,
                          iso=tuple(IsoPacket(i*64,64) for i in range(65)))
        result = await asyncio.wait_for(self.backend.submit(capture,Endpoint(0x82,1,64,1)),1)
        self.assertEqual(result.status,-95)
        self.assertTrue(self.backend.ready)

    async def test_render_waits_for_device_clock_request_before_transmit(self):
        self.backend.audio_probe = self.backend.audio_bridge = self.backend.audio_layout = True
        packet = bytes.fromhex('602001c08100')+bytes(192)
        data = packet*8
        render = replace(request(False,len(data),data),ep=2,packet_count=8,
                         iso=tuple(IsoPacket(i*198,198) for i in range(8)))
        task = asyncio.create_task(self.backend.submit(render,Endpoint(2,1,228,1)))
        await asyncio.sleep(.005)
        self.assertFalse(task.done())
        self.assertFalse(self.backend.pending)
        radio = bytes.fromhex('6020018283000006')+bytes(384)
        self.writer.write(frame(4,1,radio));await self.writer.drain()
        kind,wcid,token,data = await read_frame(self.reader)
        self.assertEqual((kind,wcid,len(data)),(18,1,1542))
        self.writer.write(frame(17,1,struct.pack('<i',0),token));await self.writer.drain()
        self.assertEqual((await task).status,0)
        self.assertEqual(self.backend.render_credits,0)

    async def test_disconnect_releases_render_waiting_for_clock(self):
        self.backend.audio_probe = self.backend.audio_bridge = self.backend.audio_layout = True
        packet = bytes.fromhex('602001c08100')+bytes(192)
        data = packet*8
        render = replace(request(False,len(data),data),ep=2,packet_count=8,
                         iso=tuple(IsoPacket(i*198,198) for i in range(8)))
        task = asyncio.create_task(self.backend.submit(render,Endpoint(2,1,228,1)))
        await asyncio.sleep(.005)
        await self.backend.reset()
        self.assertEqual((await task).status,-19)
        self.assertFalse(self.backend.pending)
        self.assertFalse(self.backend.render_pcm)

    async def test_old_headset_interrupt_and_iso_out_use_distinct_captured_channels(self):
        self.backend.interrupt_endpoints = (0x0a,0x86)
        self.backend.audio_endpoints = (0x01,0x82)
        self.backend.audio_probe = self.backend.audio_bridge = self.backend.audio_layout = True
        self.backend.render_credits = 1
        command = bytes.fromhex('04200100')
        outgoing = replace(request(False,4,command),ep=10)
        task = asyncio.create_task(self.backend.submit(outgoing,Endpoint(0x0a,3,64,4)))
        kind,wcid,token,payload = await read_frame(self.reader)
        self.assertEqual((kind,wcid,payload),(16,1,command))
        self.writer.write(frame(17,1,struct.pack('<i',0),token));await self.writer.drain()
        self.assertEqual((await task).status,0)
        self.assertEqual((await self.backend.submit(outgoing,Endpoint(0x01,3,64,4))).status,-32)
        packet = bytes.fromhex('602001c08100')+bytes(192)
        render = replace(request(False,1584,packet*8),ep=1,packet_count=8,
                         iso=tuple(IsoPacket(i*198,198) for i in range(8)))
        task = asyncio.create_task(self.backend.submit(render,Endpoint(0x01,1,576,1)))
        kind,wcid,token,payload = await read_frame(self.reader)
        self.assertEqual((kind,wcid,len(payload)),(18,1,1542))
        self.writer.write(frame(17,1,struct.pack('<i',0),token));await self.writer.drain()
        self.assertEqual((await task).status,0)


if __name__ == '__main__':
    unittest.main()

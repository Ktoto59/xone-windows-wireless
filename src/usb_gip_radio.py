# SPDX-License-Identifier: GPL-2.0-or-later
"""Native USB GIP and optional observed PCM layout over local xone radio IPC."""
import asyncio
from collections import deque
import struct
import time
from usbip_urb import Completion, IsoPacket, failed_completion


def frame(kind, wcid, payload=b'', token=0):
    if len(payload) > 1600 or not 0 <= wcid <= 16:
        raise ValueError('Invalid radio IPC frame')
    return struct.pack('<4sBBHI', b'XGP1', kind, wcid, len(payload), token)+payload


async def read_frame(reader, timeout=5):
    header = await asyncio.wait_for(reader.readexactly(12), timeout)
    magic, kind, wcid, length, token = struct.unpack('<4sBBHI', header)
    if magic != b'XGP1' or length > 1600 or wcid > 16:
        raise ValueError('Invalid radio IPC header')
    payload = await asyncio.wait_for(reader.readexactly(length), timeout)
    return kind, wcid, token, payload


def gip_packets(data):
    """Validate exact GIP frame boundaries without reassembling or rewriting."""
    result = []
    offset = 0
    def varint():
        nonlocal offset
        value = 0
        for n in range(4):
            if offset >= len(data):
                raise ValueError('Truncated GIP integer')
            part = data[offset]
            offset += 1
            value |= (part & 127) << (7*n)
            if not part & 128:
                return value
        raise ValueError('Oversized GIP integer')
    while offset < len(data):
        start = offset
        if len(data)-offset < 4:
            raise ValueError('Truncated GIP header')
        options = data[offset+1]
        offset += 3
        length = varint()
        if options & 128:
            varint()
        if length > len(data)-offset:
            raise ValueError('Truncated GIP payload')
        offset += length
        result.append(data[start:offset])
    if not result:
        raise ValueError('Empty GIP transfer')
    return result


def gip_header_size(packet):
    offset = 3
    for _ in range(2 if packet[1] & 128 else 1):
        while packet[offset] & 128:
            offset += 1
        offset += 1
    return offset


def encode_chunk(command, options, sequence, payload, chunk_offset):
    def integer(value):
        result = bytearray()
        while True:
            result.append((value & 127) | (128 if value > 127 else 0))
            value >>= 7
            if not value:
                return result
    length,offset = integer(len(payload)),integer(chunk_offset)
    if (3+len(length)+len(offset)) % 2:
        length[-1] |= 128
        length.append(0)
    return bytes((command,options,sequence))+length+offset+payload


def usb_interrupt_packets(data):
    """Split RF chunks to USB size without changing logical offsets/ACKs.

    Only the last subchunk requests the original ACK. Thus the USB driver's
    acknowledgement retains the RF chunk's complete byte count and remainder.
    """
    result = []
    for packet in gip_packets(data):
        if packet[0] == 0x60:
            raise ValueError('Audio cannot enter USB interrupt endpoint')
        if len(packet) <= 64:
            result.append(packet)
            continue
        command,options,sequence = packet[:3]
        if not options & 128:
            raise ValueError('Oversized unfragmented radio GIP packet')
        offset = 3
        def integer():
            nonlocal offset
            value = 0
            for shift in range(0,28,7):
                part = packet[offset];offset += 1
                value |= (part & 127) << shift
                if not part & 128:
                    return value
            raise ValueError('Oversized chunk integer')
        length,chunk_offset = integer(),integer()
        payload = packet[offset:]
        if length != len(payload) or chunk_offset > 65535 or (options & 64 and chunk_offset < length):
            raise ValueError('Invalid radio GIP chunk range')
        position = 0 if options & 64 else chunk_offset
        if position+length > 65535:
            raise ValueError('Radio chunk exceeds logical transfer limit')
        for at in range(0,length,56):
            body = payload[at:at+56]
            part_options = options & ~(16|64)
            part_offset = position+at
            if not at and options & 64:
                part_options |= 64
                part_offset = chunk_offset
            if at+len(body) == length:
                part_options |= options & 16
            part = encode_chunk(command,part_options,sequence,body,part_offset)
            if len(part) > 64:
                raise ValueError('Split radio chunk exceeds USB size')
            result.append(part)
    return result


class RadioBackend:
    def __init__(self, mac, on_event=None, audio_probe=False, audio_bridge=False,
                 interrupt_endpoints=(0x01,0x81), audio_endpoints=(0x02,0x82)):
        self.mac = bytes.fromhex(mac)
        if len(self.mac) != 6:
            raise ValueError('Expected a six-byte radio MAC')
        self.on_event = on_event
        self.interrupt_endpoints = interrupt_endpoints
        self.audio_endpoints = audio_endpoints
        self.writer = None
        self.wcid = 0
        self.generation = 0
        self.queue = deque()
        self.audio_queue = deque()
        self.audio_probe = audio_probe or audio_bridge
        self.audio_bridge = audio_bridge
        self.audio_layout = False
        self.render_pcm = bytearray()
        self.audio_sequence = 1
        self.capture_sequence = 1
        self.audio_tx = 0
        self.audio_rx = self.audio_out_probes = 0
        self.changed = asyncio.Condition()
        self.capture_delivery = asyncio.Lock()
        self.render_delivery = asyncio.Lock()
        self.render_credits = 0
        self.capture_deadline = 0
        self.usb_audio_in = 0
        self.pending = {}
        self.token = 0
        self.rx_packets = self.tx_packets = 0
        self.battery = None

    @property
    def ready(self):
        return self.writer is not None and self.wcid != 0

    def event(self, **fields):
        if self.on_event:
            self.on_event(fields)

    async def reset(self):
        self.wcid = 0
        self.generation += 1
        self.queue.clear()
        self.audio_queue.clear()
        self.render_pcm.clear()
        self.audio_layout = False
        self.audio_sequence = 1
        self.capture_sequence = 1
        self.capture_deadline = 0
        self.render_credits = 0
        self.battery = None
        for future in self.pending.values():
            if not future.done():
                future.set_result(-19)
        async with self.changed:
            self.changed.notify_all()

    def control_changed(self, setup):
        if setup[:2] == b'\x00\x09' or (setup[:2] == b'\x01\x0b' and setup[2:6] == b'\x00\x00\x01\x00'):
            self.clear_audio()

    def clear_audio(self):
        self.render_pcm.clear()
        self.audio_queue.clear()
        self.audio_layout = False
        self.capture_deadline = 0
        self.render_credits = 0

    def usb_capture_block(self, packet):
        pcm = packet[gip_header_size(packet)+2:]
        if len(pcm) != 384:
            raise ValueError('Unsupported radio capture block')
        frames = []
        for offset in range(0,384,48):
            # USB GIP consumes one millisecond at a time. Its 48-byte mono
            # input fragment requests a 192-byte stereo output fragment.
            frames.append(bytes((0x60,32,self.capture_sequence,50,192,0))+pcm[offset:offset+48])
            self.capture_sequence = self.capture_sequence % 255 + 1
        return frames

    async def connection(self, reader, writer):
        if self.writer is not None:
            writer.close()
            await writer.wait_closed()
            return
        owns_connection = False
        # Reserve the connection before awaiting its handshake.
        self.writer = writer
        owns_connection = True
        try:
            kind, wcid, token, payload = await read_frame(reader)
            if (kind, wcid, token, payload) != (0, 0, 0, self.mac):
                raise ValueError('Radio IPC MAC handshake mismatch')
            self.event(operation='radio_ready')
            while True:
                # Radio can remain associated and silent; lifetime is bounded
                # by the outer server/bridge, not a five-second idle timeout.
                kind, wcid, token, payload = await read_frame(reader, 3600)
                if kind == 1 and 1 <= wcid <= 16 and token == 0 and payload == self.mac:
                    await self.reset()
                    self.wcid = wcid
                    self.event(operation='radio_associated', wcid=wcid)
                elif kind == 2 and wcid == self.wcid and wcid != 0 and token == 0:
                    radio_packets = gip_packets(payload)
                    packets = usb_interrupt_packets(payload)
                    if len(packets) != len(radio_packets):
                        self.event(operation='radio_gip_split', commands=[p[0] for p in radio_packets],
                                   radio_lengths=[len(p) for p in radio_packets],usb_lengths=[len(p) for p in packets])
                    if len(self.queue)+len(packets) > 256:
                        raise ValueError('USB interrupt queue limit')
                    self.queue.extend(packets)
                    # Observe complete parent status only; forward every byte
                    # unchanged. USB transport reporting remains driver-owned.
                    for p in radio_packets:
                        if p[0] == 3 and p[1] == 32:
                            body = p[gip_header_size(p):]
                            if len(body) >= 4:
                                value = body[0]
                                power_type,level = (value >> 2) & 3,value & 3
                                self.battery = {'Connected': bool(value & 128),
                                                'Type': ('none','standard','play-and-charge','unknown')[power_type],
                                                'Level': ('low','normal','high','full')[level] if power_type else 'unknown',
                                                'RawType': power_type,'RawLevel': level}
                    self.rx_packets += len(radio_packets)
                    self.event(operation='radio_gip_in', commands=[p[0] for p in packets], lengths=[len(p) for p in packets])
                    async with self.changed:
                        self.changed.notify_all()
                elif kind == 3 and wcid == self.wcid and token == 0 and not payload:
                    await self.reset()
                    self.event(operation='radio_removed', wcid=wcid)
                elif kind == 4 and wcid == self.wcid and wcid != 0 and token == 0:
                    packets = gip_packets(payload)
                    if any(p[0] != 0x60 for p in packets):
                        raise ValueError('Non-audio packet on radio audio channel')
                    self.audio_rx += len(packets)
                    self.audio_layout = all(len(p[gip_header_size(p):]) == 386 and
                                            p[gip_header_size(p):gip_header_size(p)+2] == b'\0\x06' for p in packets)
                    if self.audio_probe:
                        # Bounded volatile queue, no PCM is written to journals.
                        queued_frames = len(packets)*(8 if self.audio_bridge else 1)
                        queue_limit = 2048 if self.audio_bridge else 256
                        if len(self.audio_queue)+queued_frames > queue_limit:
                            raise ValueError('Radio audio queue limit')
                        if self.audio_bridge:
                            if not self.audio_layout:
                                raise ValueError('Unsupported radio PCM layout')
                            # Each ISO completion carries one GIP packet. The
                            # native USB driver consumes only the first packet
                            # when multiple GIP packets share one ISO slot.
                            for p in packets:
                                self.audio_queue.extend(self.usb_capture_block(p))
                            # length_out is the device's request for its next
                            # 8-ms render block. Never run ahead of its clock;
                            # retain at most two requests to bound burst latency.
                            self.render_credits = min(2,self.render_credits+len(packets))
                        else:
                            self.audio_queue.extend(packets)
                        async with self.changed:
                            self.changed.notify_all()
                    if self.audio_rx <= 8 or self.audio_rx % 500 == 0:
                        self.event(operation='radio_audio_in', lengths=[len(p) for p in packets],
                                   output_lengths=[int.from_bytes(p[gip_header_size(p):gip_header_size(p)+2], 'little') for p in packets],
                                   packets=self.audio_rx)
                elif kind == 17 and len(payload) == 4 and token != 0:
                    future = self.pending.get(token)
                    if future is not None and not future.done():
                        if wcid != self.wcid:
                            future.set_result(-19)
                        else:
                            future.set_result(struct.unpack('<i', payload)[0])
                else:
                    raise ValueError('Unexpected radio IPC message')
        except (asyncio.IncompleteReadError, ConnectionError, TimeoutError, ValueError) as error:
            self.event(operation='radio_closed', reason=type(error).__name__, detail=str(error))
        finally:
            if owns_connection:
                self.writer = None
                await self.reset()
                self.event(operation='radio_stopped')
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

    async def submit(self, request, endpoint):
        if self.audio_probe and endpoint.address in self.audio_endpoints and request.iso:
            return await self.probe_audio(request)
        if endpoint.address not in self.interrupt_endpoints or request.iso:
            return failed_completion(request, -32)
        if not self.ready:
            return failed_completion(request, -19)
        generation = self.generation
        if request.direction:
            async with self.changed:
                await self.changed.wait_for(lambda: self.queue or not self.ready or self.generation != generation)
                if not self.ready or self.generation != generation:
                    return failed_completion(request, -19)
                if len(self.queue[0]) > request.length:
                    return failed_completion(request, -90)
                data = self.queue.popleft()
            return Completion(data=data, actual=len(data))
        try:
            packets = gip_packets(request.data)
            if request.length > 64 or any(p[0] == 0x60 for p in packets):
                return failed_completion(request, -95)
        except ValueError:
            return failed_completion(request, -71)
        if len(self.pending) >= 64:
            return failed_completion(request, -12)
        self.token = self.token % 0xffffffff + 1
        while self.token in self.pending:
            self.token = self.token % 0xffffffff + 1
        token = self.token
        future = asyncio.get_running_loop().create_future()
        self.pending[token] = future
        try:
            self.writer.write(frame(16, self.wcid, request.data, token))
            await asyncio.wait_for(self.writer.drain(), 5)
            status = await asyncio.wait_for(future, 5)
            if self.generation != generation:
                status = -19
            if status == 0:
                self.tx_packets += len(packets)
            self.event(operation='radio_gip_out', commands=[p[0] for p in packets], lengths=[len(p) for p in packets], status=status)
            return Completion(actual=request.length) if status == 0 else failed_completion(request, status)
        except (ConnectionError, TimeoutError, AttributeError):
            return failed_completion(request, -19)
        finally:
            self.pending.pop(token, None)

    async def probe_audio(self, request):
        """Capture-path experiment; render OUT deliberately remains unsupported."""
        if not self.ready:
            return failed_completion(request, -19)
        generation = self.generation
        if not request.direction:
            if self.audio_bridge:
                return await self.render_audio(request)
            self.audio_out_probes += 1
            if self.audio_out_probes <= 16:
                headers = []
                for part in request.iso:
                    data = request.data[part.offset:part.offset+part.length]
                    try:
                        packets = gip_packets(data)
                        headers.extend({'command': p[0], 'options': p[1], 'sequence': p[2], 'length': len(p)} for p in packets)
                    except ValueError:
                        headers.append({'invalid': True, 'length': len(data)})
                self.event(operation='usb_audio_out_probe', capacity=request.length,
                           slots=[p.length for p in request.iso], headers=headers[:32])
            # Avoid the native driver's immediate error/resubmit busy loop.
            await asyncio.sleep(.008)
            return failed_completion(request, -95)
        async with self.capture_delivery:
            count = len(request.iso) if self.audio_bridge else 1
            async with self.changed:
                await self.changed.wait_for(lambda: len(self.audio_queue) >= count or not self.ready or self.generation != generation)
                if not self.ready or self.generation != generation:
                    return failed_completion(request, -19)
                if any(len(self.audio_queue[i]) > request.iso[i].length for i in range(count)):
                    return failed_completion(request, -90)
            if self.audio_bridge:
                delay = self.capture_deadline-time.perf_counter()
                if delay > 0:
                    # Python's Windows sleep uses a high-resolution waitable
                    # timer; a worker keeps command IPC responsive meanwhile.
                    await asyncio.to_thread(time.sleep, delay)
                if not self.ready or self.generation != generation or len(self.audio_queue) < count:
                    return failed_completion(request, -19)
                duration = .001*count
                self.capture_deadline = max(self.capture_deadline+duration,time.perf_counter()-duration)
            packets = [self.audio_queue.popleft() for _ in range(count)]
            self.usb_audio_in += count
        result = tuple(IsoPacket(p.offset, p.length, actual=len(packets[i]) if i < count else 0) for i, p in enumerate(request.iso))
        data = b''.join(packets)
        return Completion(data=data, actual=len(data), iso=result, frame=request.frame)

    async def render_audio(self, request):
        generation = self.generation
        async with self.render_delivery:
            if not self.ready or generation != self.generation:
                return failed_completion(request,-19)
            return await self._render_audio(request)

    async def _render_audio(self, request):
        if not self.audio_layout:
            return failed_completion(request, -95)
        fragments = []
        try:
            for slot in request.iso:
                data = request.data[slot.offset:slot.offset+slot.length]
                for packet in gip_packets(data):
                    pcm = packet[gip_header_size(packet):]
                    if packet[0] != 0x60 or packet[1] != 32 or len(pcm) != 192:
                        return failed_completion(request, -95)
                    fragments.append(pcm)
        except ValueError:
            return failed_completion(request, -71)
        if len(self.render_pcm)+192*len(fragments) > 1536*8 or len(self.pending) >= 64:
            return failed_completion(request, -12)
        generation = self.generation
        self.render_pcm.extend(b''.join(fragments))
        while len(self.render_pcm) >= 1536:
            try:
                async with self.changed:
                    await asyncio.wait_for(self.changed.wait_for(lambda: self.render_credits or not self.ready or self.generation != generation),5)
                    if not self.ready or self.generation != generation:
                        return failed_completion(request,-19)
                    self.render_credits -= 1
            except TimeoutError:
                if self.generation == generation:
                    await self.reset()
                return failed_completion(request,-19)
            pcm = bytes(self.render_pcm[:1536])
            del self.render_pcm[:1536]
            # Even GIP header: varint 1536 with a terminal zero padding group.
            packet = bytes((0x60,32,self.audio_sequence,0x80,0x8c,0))+pcm
            self.audio_sequence = self.audio_sequence % 255 + 1
            self.token = self.token % 0xffffffff + 1
            while self.token in self.pending:
                self.token = self.token % 0xffffffff + 1
            token = self.token
            future = asyncio.get_running_loop().create_future()
            self.pending[token] = future
            try:
                self.writer.write(frame(18, self.wcid, packet, token))
                await asyncio.wait_for(self.writer.drain(), 5)
                status = await asyncio.wait_for(future, 5)
                if self.generation != generation:
                    status = -19
                if status != 0:
                    if self.generation == generation:
                        await self.reset()
                    return failed_completion(request, status)
                self.audio_tx += 1
                if self.audio_tx <= 8 or self.audio_tx % 500 == 0:
                    self.event(operation='radio_audio_out', packets=self.audio_tx, pcm_bytes=1536)
            except (ConnectionError, TimeoutError, AttributeError):
                if self.generation == generation:
                    await self.reset()
                return failed_completion(request, -19)
            finally:
                self.pending.pop(token, None)
        # Short USB fragments are accepted into the bounded 8-ms accumulator.
        # A completed radio block is acknowledged only after actual USB TX.
        iso = tuple(IsoPacket(p.offset,p.length,p.length) for p in request.iso)
        return Completion(actual=sum(p.actual for p in iso), iso=iso, frame=request.frame)

    def status(self):
        return {'RadioConnected': self.writer is not None, 'Wcid': self.wcid,
                'Generation': self.generation,
                'Ready': self.ready, 'QueuedPackets': len(self.queue),
                'PendingWrites': len(self.pending), 'RxPackets': self.rx_packets,
                'TxPackets': self.tx_packets, 'AudioRxPackets': self.audio_rx,
                'AudioOutProbeRequests': self.audio_out_probes,
                'AudioProbe': self.audio_probe, 'AudioBridge': self.audio_bridge,
                'AudioLayoutSupported': self.audio_layout, 'AudioTxPackets': self.audio_tx,
                'RenderBufferedBytes': len(self.render_pcm), 'RenderCredits': self.render_credits,
                'IsoAudioImplemented': self.audio_bridge,
                'UsbAudioInCompletions': self.usb_audio_in, 'QueuedAudioFrames': len(self.audio_queue),
                'RadioBattery': self.battery}

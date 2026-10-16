# SPDX-License-Identifier: GPL-2.0-or-later
"""Native USB GIP and optional observed PCM layout over local xone radio IPC."""
import asyncio
from collections import deque
import struct
import time
import development_trace as trace
from usbip_urb import Completion, IsoPacket, failed_completion

# Four observed 8-ms radio blocks: a recovery budget, not an added delay.
AUDIO_MAX_AGE = .032
CAPTURE_MAX_FRAMES = 32
# Native XboxGip keeps several future 8-ms OUT URBs queued. Its observed
# ordinary queue wait is ~35 ms; allow that pipeline, expire actual stalls.
RENDER_MAX_AGE = .064
# Experimental independent playback clock: tolerate a short microphone RF
# outage, then wait for fresh radio activity. This is not a PCM queue budget.
CLOCK_HOLDOVER = .128
# Known USB initialization forms. The second was observed from XboxGip on
# headset 2021; preserve its unknown fields rather than generalizing them.
USB_WIRED_MODE_PAYLOADS = (bytes((6,))+bytes(14),
                           bytes((6,))+bytes.fromhex('0000000000005553000000000000'))


def frame(kind, wcid, payload=b'', token=0):
    if len(payload) > 1600 or not 0 <= wcid <= 16:
        raise ValueError('Invalid radio IPC frame')
    trace.event('ipc_frame_encoded', kind=kind, wcid=wcid, length=len(payload), token=token)
    return struct.pack('<4sBBHI', b'XGP1', kind, wcid, len(payload), token)+payload


async def read_frame(reader, timeout=5):
    started = time.perf_counter_ns()
    trace.event('ipc_read_started', timeout_seconds=timeout)
    header = await asyncio.wait_for(reader.readexactly(12), timeout)
    magic, kind, wcid, length, token = struct.unpack('<4sBBHI', header)
    if magic != b'XGP1' or length > 1600 or wcid > 16:
        raise ValueError('Invalid radio IPC header')
    payload = await asyncio.wait_for(reader.readexactly(length), timeout)
    trace.event('ipc_read_complete', kind=kind, wcid=wcid, length=length, token=token,
                elapsed_us=(time.perf_counter_ns()-started)/1000)
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
                 interrupt_endpoints=(0x01,0x81), audio_endpoints=(0x02,0x82),
                 clock=time.perf_counter, render_max_age=RENDER_MAX_AGE,
                 independent_clock=False, on_announce=None, preserve_wireless_mode=False):
        self.mac = bytes.fromhex(mac)
        if len(self.mac) != 6:
            raise ValueError('Expected a six-byte radio MAC')
        self.on_event = on_event
        self.on_announce = on_announce
        self.power_observations = 0
        self.preserve_wireless_mode = preserve_wireless_mode
        self.usb_mode_switch_suppressed = 0
        self.clock = clock
        self.render_max_age = render_max_age
        self.independent_clock = independent_clock
        self.render_deadline = 0
        self.capture_concealed = 0
        self.interrupt_endpoints = interrupt_endpoints
        self.audio_endpoints = audio_endpoints
        self.writer = None
        self.wcid = 0
        self.generation = 0
        self.queue = deque()
        self.announce = None
        self.usb_imports = 0
        self.audio_queue = deque()
        self.audio_probe = audio_probe or audio_bridge
        self.audio_bridge = audio_bridge
        self.audio_layout = False
        self.render_pcm = bytearray()
        self.render_started = None
        self.audio_epoch = 0
        self.capture_dropped = self.render_dropped = self.render_discarded_bytes = 0
        self.capture_high_water = 0
        self.render_max_wait = self.radio_ack_max_wait = 0
        self.render_requests_pending = self.render_requests_high_water = 0
        self.render_request_count = self.render_fragment_count = 0
        self.radio_audio_last = None
        self.radio_audio_max_gap = 0
        self.render_credit_overflow = 0
        self.audio_sequence = 1
        self.capture_sequence = 1
        self.audio_tx = 0
        self.audio_rx = self.audio_out_probes = 0
        self.changed = asyncio.Condition()
        self.capture_delivery = asyncio.Lock()
        self.render_delivery = asyncio.Lock()
        self.render_credits = 0
        self.render_credit_time = None
        self.capture_deadline = 0
        self.usb_audio_in = 0
        self.pending = {}
        self.token = 0
        self.rx_packets = self.tx_packets = 0
        self.battery = None
        self.recovery_required = False

    @property
    def ready(self):
        return self.importable and not self.recovery_required

    @property
    def importable(self):
        return self.writer is not None and self.wcid != 0

    def imported(self):
        self.clear_audio()
        self.recovery_required = False
        if self.usb_imports:
            self.queue.clear()
            if self.announce is not None:
                self.queue.append(self.announce)
        self.usb_imports += 1

    async def recover_audio(self):
        announce, imports = self.announce, self.usb_imports
        await self.reset(recover_usb=True)
        self.announce, self.usb_imports = announce, imports
        trace.event('audio_recovery_required', suspicious=True, wcid=self.wcid, generation=self.generation)

    def event(self, **fields):
        if self.on_event:
            self.on_event(fields)

    async def reset(self, recover_usb=False):
        self.recovery_required = bool(recover_usb and self.wcid)
        if not recover_usb:
            self.wcid = 0
        self.generation += 1
        self.queue.clear()
        self.announce = None
        self.usb_imports = 0
        self.clear_audio()
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
        trace.event('audio_reset', epoch=self.audio_epoch, capture_frames=len(self.audio_queue),
                    render_bytes=len(self.render_pcm), pending_writes=len(self.pending))
        self.audio_epoch += 1
        self.render_pcm.clear()
        self.render_started = None
        self.audio_queue.clear()
        self.audio_layout = False
        self.capture_deadline = 0
        self.render_credits = 0
        self.render_credit_time = None
        self.radio_audio_last = None
        self.render_deadline = 0

    def clock_active(self):
        return (self.independent_clock and self.audio_layout and self.radio_audio_last is not None
                and self.clock()-self.radio_audio_last <= CLOCK_HOLDOVER)

    def trim_capture(self):
        now = self.clock()
        limit = CAPTURE_MAX_FRAMES if self.audio_bridge else 4
        while self.audio_queue and (len(self.audio_queue) > limit or
                                   now-self.audio_queue[0][0] > AUDIO_MAX_AGE):
            trace.event('capture_frame_dropped', suspicious=True, queued=len(self.audio_queue),
                        age_ms=(now-self.audio_queue[0][0])*1000, capacity=limit,
                        reason='capacity' if len(self.audio_queue)>limit else 'expired')
            self.audio_queue.popleft()
            self.capture_dropped += 1
        self.capture_high_water = max(self.capture_high_water, len(self.audio_queue))

    def discard_render(self, request):
        trace.event('render_expired', suspicious=True, seq=request.seq, pending=self.render_requests_pending,
                    buffered_bytes=len(self.render_pcm), budget_ms=self.render_max_age*1000)
        self.render_dropped += 1
        self.render_discarded_bytes += len(self.render_pcm)
        self.render_pcm.clear()
        self.render_started = None
        # An expired ISO packet was not transmitted. usbip-win2 maps EXDEV
        # to ISO_TD_ERROR and consumes per-packet error_count normally.
        iso = tuple(IsoPacket(p.offset, p.length, status=-18) for p in request.iso)
        return Completion(iso=iso, frame=request.frame)

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

    async def connection(self, reader, writer, handshake=None):
        if self.writer is not None:
            writer.close()
            await writer.wait_closed()
            return
        owns_connection = False
        # Reserve the connection before awaiting its handshake.
        self.writer = writer
        owns_connection = True
        try:
            kind, wcid, token, payload = handshake if handshake is not None else await read_frame(reader)
            if (kind, wcid, token, payload) != (0, 0, 0, self.mac):
                raise ValueError('Radio IPC MAC handshake mismatch')
            self.event(operation='radio_ready')
            while True:
                # An idle headset must not end an unlimited bridge after an
                # hour. EOF and outer cancellation still close this channel.
                kind, wcid, token, payload = await read_frame(reader, None)
                if kind == 1 and 1 <= wcid <= 16 and token == 0 and payload == self.mac:
                    await self.reset()
                    self.wcid = wcid
                    self.event(operation='radio_associated', wcid=wcid)
                elif kind == 2 and wcid == self.wcid and wcid != 0 and token == 0:
                    radio_packets = gip_packets(payload)
                    for p in radio_packets:
                        body = p[gip_header_size(p):]
                        if p[0] == 2 and p[1] == 32 and len(body) >= 28 and len(p) <= 64 and body[8:10] == b'\x5e\x04':
                            self.announce = p
                    if self.on_announce is not None:
                        for p in radio_packets:
                            body = p[gip_header_size(p):]
                            if p[0] == 2 and p[1] & 0x3f == 32 and len(body) >= 28:
                                self.on_announce(*struct.unpack_from('<HH',body,8))
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
                        arrived = self.clock()
                        if self.radio_audio_last is not None:
                            gap = arrived-self.radio_audio_last
                            self.radio_audio_max_gap = max(self.radio_audio_max_gap, gap)
                            trace.event('radio_audio_gap', suspicious=gap>AUDIO_MAX_AGE, gap_ms=gap*1000)
                        self.radio_audio_last = arrived
                        if self.audio_bridge:
                            if not self.audio_layout:
                                raise ValueError('Unsupported radio PCM layout')
                            # Each ISO completion carries one GIP packet. The
                            # native USB driver consumes only the first packet
                            # when multiple GIP packets share one ISO slot.
                            for p in packets:
                                self.audio_queue.extend((arrived, p) for p in self.usb_capture_block(p))
                            # length_out is the device's request for its next
                            # 8-ms render block. Never run ahead of its clock;
                            # retain at most two requests to bound burst latency.
                            if not self.independent_clock:
                                credits = self.render_credits+len(packets)
                                if credits > 2:
                                    trace.event('render_credit_overflow', suspicious=True, credits=credits, capacity=2)
                                self.render_credit_overflow += max(0,credits-2)
                                self.render_credits = min(2,credits)
                            self.render_credit_time = arrived
                        else:
                            self.audio_queue.extend((arrived, p) for p in packets)
                        # PCM overflow/expiry is local media loss, never a
                        # reason to close the association or discard commands.
                        self.trim_capture()
                        async with self.changed:
                            self.changed.notify_all()
                    if self.audio_rx <= 8 or self.audio_rx % 500 == 0:
                        self.event(operation='radio_audio_in', lengths=[len(p) for p in packets],
                                   output_lengths=[int.from_bytes(p[gip_header_size(p):gip_header_size(p)+2], 'little') for p in packets],
                                   packets=self.audio_rx)
                elif kind == 17 and len(payload) == 4 and token != 0:
                    future = self.pending.get(token)
                    status = struct.unpack('<i', payload)[0]
                    trace.event('radio_write_ack', suspicious=status!=0 or future is None or wcid!=self.wcid,
                                token=token, wcid=wcid, status=status, pending=len(self.pending))
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
                await asyncio.wait_for(writer.wait_closed(),1)
            except (ConnectionError,TimeoutError):
                writer.transport.abort()

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
        forwarded = []
        for packet in packets:
            # Experimental RF/USB adaptation. This non-ACK parent power command
            # belongs to USB initialization; never consume auth, chunks, child
            # commands or requests requiring a GIP ACK. Default remains pass-through.
            if (self.preserve_wireless_mode and packet[:2]==bytes((5,32)) and
                    packet[gip_header_size(packet):] in USB_WIRED_MODE_PAYLOADS):
                self.usb_mode_switch_suppressed += 1
                self.event(operation='usb_mode_switch_suppressed',mode=6,payload_length=15)
            else:
                forwarded.append(packet)
        if not forwarded:
            return Completion(actual=request.length)
        if len(self.pending) >= 64:
            return failed_completion(request, -12)
        self.token = self.token % 0xffffffff + 1
        while self.token in self.pending:
            self.token = self.token % 0xffffffff + 1
        token = self.token
        future = asyncio.get_running_loop().create_future()
        self.pending[token] = future
        try:
            self.writer.write(frame(16, self.wcid, b''.join(forwarded), token))
            await asyncio.wait_for(self.writer.drain(), 5)
            status = await asyncio.wait_for(future, 5)
            if self.generation != generation:
                status = -19
            if status == 0:
                self.tx_packets += len(forwarded)
            self.event(operation='radio_gip_out', commands=[p[0] for p in forwarded], lengths=[len(p) for p in forwarded], status=status)
            for packet in forwarded:
                # Observe only the mode byte, never authentication material/PCM.
                if packet[0]==5 and packet[1]&0x3f==0x20 and not packet[1]&0x80 and self.power_observations<16:
                    body = packet[gip_header_size(packet):]
                    self.power_observations += 1
                    fields=dict(operation='radio_power_out',mode=body[0] if body else None,
                                payload_length=len(body),tail_zero=not any(body[1:]),status=status)
                    if body and body[0]==6:fields['parameters']=body[1:].hex()
                    self.event(**fields)
            return Completion(actual=request.length) if status == 0 else failed_completion(request, status)
        except (ConnectionError, TimeoutError, AttributeError):
            return failed_completion(request, -19)
        finally:
            self.pending.pop(token, None)

    async def probe_audio(self, request):
        """Experimental ISO capture and optional observed PCM render bridge."""
        if not self.ready:
            return failed_completion(request, -19)
        generation = self.generation
        epoch = self.audio_epoch
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
            if count > CAPTURE_MAX_FRAMES:
                return failed_completion(request, -22)
            while True:
                async with self.changed:
                    self.trim_capture()
                    await self.changed.wait_for(lambda: len(self.audio_queue) >= count or self.clock_active() or not self.ready or
                                                self.generation != generation or self.audio_epoch != epoch)
                    if not self.ready or self.generation != generation or self.audio_epoch != epoch:
                        return failed_completion(request, -19)
                    # The wait can resume after a scheduler stall.
                    self.trim_capture()
                    if len(self.audio_queue) < count and not self.clock_active():
                        continue
                    if any(len(self.audio_queue[i][1]) > request.iso[i].length for i in range(min(count,len(self.audio_queue)))):
                        return failed_completion(request, -90)
                    if self.clock_active() and any(p.length < 54 for p in request.iso[:count]):
                        return failed_completion(request, -90)
                if self.audio_bridge:
                    if self.independent_clock and not self.capture_deadline:
                        # One RF block of jitter reserve, then a continuous USB
                        # sample clock. Missing microphone PCM becomes silence.
                        self.capture_deadline = self.clock()+.008
                    delay = self.capture_deadline-self.clock()
                    if delay > 0:
                        # Windows high-resolution sleep, outside the RF loop.
                        await asyncio.to_thread(time.sleep, delay)
                    if not self.ready or self.generation != generation or self.audio_epoch != epoch:
                        return failed_completion(request, -19)
                    self.trim_capture()
                    if len(self.audio_queue) < count and not self.clock_active():
                        continue
                    duration = .001*count
                    self.capture_deadline = max(self.capture_deadline+duration,self.clock()-duration)
                packets = [self.audio_queue.popleft()[1] for _ in range(min(count,len(self.audio_queue)))]
                concealed = max(0, count-len(packets))
                if concealed:
                    trace.event('capture_concealment', suspicious=True, seq=request.seq, frames=concealed)
                while len(packets) < count:
                    packets.append(bytes((0x60,32,self.capture_sequence,50,192,0))+bytes(48))
                    self.capture_sequence = self.capture_sequence % 255 + 1
                    self.capture_concealed += 1
                break
            self.usb_audio_in += count
        result = tuple(IsoPacket(p.offset, p.length, actual=len(packets[i]) if i < count else 0) for i, p in enumerate(request.iso))
        data = b''.join(packets)
        return Completion(data=data, actual=len(data), iso=result, frame=request.frame)

    async def render_audio(self, request):
        self.render_requests_pending += 1
        self.render_requests_high_water = max(self.render_requests_high_water,self.render_requests_pending)
        self.render_request_count += 1
        try:
            return await self.queued_render_audio(request)
        finally:
            self.render_requests_pending -= 1

    async def queued_render_audio(self, request):
        generation = self.generation
        epoch = self.audio_epoch
        arrived = self.clock()
        async with self.render_delivery:
            if not self.ready or generation != self.generation or epoch != self.audio_epoch:
                return failed_completion(request,-19)
            waited = self.clock()-arrived
            trace.event('render_queue_wait', suspicious=waited>self.render_max_age, seq=request.seq,
                        waited_ms=waited*1000, pending=self.render_requests_pending,
                        budget_ms=self.render_max_age*1000)
            self.render_max_wait = max(self.render_max_wait, waited)
            if waited > self.render_max_age:
                return self.discard_render(request)
            if self.render_started is not None and self.clock()-self.render_started > self.render_max_age:
                trace.event('render_buffer_expired', suspicious=True, seq=request.seq,
                            buffered_bytes=len(self.render_pcm))
                self.render_discarded_bytes += len(self.render_pcm)
                self.render_pcm.clear()
                self.render_started = None
            try:
                return await self._render_audio(request, arrived, epoch)
            except asyncio.CancelledError:
                # UNLINK must not leave unsent PCM for a later URB to replay.
                if self.audio_epoch == epoch:
                    trace.event('render_cancelled', seq=request.seq, buffered_bytes=len(self.render_pcm))
                    self.render_discarded_bytes += len(self.render_pcm)
                    self.render_pcm.clear()
                    self.render_started = None
                raise

    async def _render_audio(self, request, arrived, epoch):
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
        self.render_fragment_count += len(fragments)
        if len(self.render_pcm)+192*len(fragments) > 192*CAPTURE_MAX_FRAMES or len(self.pending) >= 64:
            trace.event('render_queue_full', suspicious=True, seq=request.seq,
                        buffered_bytes=len(self.render_pcm), pending=len(self.pending), fragments=len(fragments))
            return failed_completion(request, -12)
        generation = self.generation
        if not self.render_pcm:
            self.render_started = arrived
        deadline = min(arrived, self.render_started)+self.render_max_age
        self.render_pcm.extend(b''.join(fragments))
        while len(self.render_pcm) >= 1536:
            try:
                async with self.changed:
                    if self.render_credit_time is not None and self.clock()-self.render_credit_time > AUDIO_MAX_AGE:
                        self.render_credits = 0
                    await asyncio.wait_for(self.changed.wait_for(lambda: (self.clock_active() if self.independent_clock else self.render_credits) or not self.ready or
                                               self.generation != generation or self.audio_epoch != epoch),
                                           max(.000001, deadline-self.clock()))
                    if not self.ready or self.generation != generation or self.audio_epoch != epoch:
                        return failed_completion(request,-19)
                    if self.clock() > deadline:
                        return self.discard_render(request)
                    if not self.independent_clock:
                        self.render_credits -= 1
            except TimeoutError:
                if not self.ready or self.generation != generation or self.audio_epoch != epoch:
                    return failed_completion(request,-19)
                # asyncio's timer and the injected media clock are separate;
                # always decide expiry from the media deadline, not a wakeup.
                if self.clock() < deadline:
                    continue
                return self.discard_render(request)
            if self.independent_clock:
                delay = self.render_deadline-self.clock()
                if delay > 0:
                    await asyncio.to_thread(time.sleep,delay)
                if not self.ready or self.generation != generation or self.audio_epoch != epoch:
                    return failed_completion(request,-19)
                if self.clock() > deadline or not self.clock_active():
                    return self.discard_render(request)
                # Absolute pacing with at most one block of catch-up, never a
                # burst replay of all the ticks missed during a scheduler stall.
                self.render_deadline = (max(self.render_deadline+.008,self.clock())
                                        if self.render_deadline else self.clock()+.008)
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
                sent = self.clock()
                self.writer.write(frame(18, self.wcid, packet, token))
                await asyncio.wait_for(self.writer.drain(), 5)
                status = await asyncio.wait_for(future, 5)
                waited = self.clock()-sent
                self.radio_ack_max_wait = max(self.radio_ack_max_wait, waited)
                trace.event('radio_audio_ack_wait', suspicious=waited>.020 or status!=0,
                            token=token, seq=request.seq, waited_ms=waited*1000, status=status)
                if self.generation != generation or self.audio_epoch != epoch:
                    status = -19
                if status == -18:
                    # Explicit unsent-media expiry is not a radio TX failure.
                    return self.discard_render(request)
                if status != 0:
                    if self.generation == generation and self.audio_epoch == epoch:
                        await self.recover_audio()
                    return failed_completion(request, status)
                self.audio_tx += 1
                if self.audio_tx <= 8 or self.audio_tx % 500 == 0:
                    self.event(operation='radio_audio_out', packets=self.audio_tx, pcm_bytes=1536)
            except (ConnectionError, TimeoutError, AttributeError):
                if self.generation == generation and self.audio_epoch == epoch:
                    await self.recover_audio()
                return failed_completion(request, -19)
            finally:
                self.pending.pop(token, None)
        if not self.render_pcm:
            self.render_started = None
        # Short USB fragments are accepted into the bounded 8-ms accumulator.
        # A completed radio block is acknowledged only after actual USB TX.
        iso = tuple(IsoPacket(p.offset,p.length,p.length) for p in request.iso)
        return Completion(actual=sum(p.actual for p in iso), iso=iso, frame=request.frame)

    def status(self):
        return {'RadioConnected': self.writer is not None, 'Wcid': self.wcid,
                'RadioAssociated': self.importable, 'RecoveryRequired': self.recovery_required,
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
                'AudioMaxAgeMs': int(AUDIO_MAX_AGE*1000), 'CaptureQueueHighWater': self.capture_high_water,
                'RenderMaxAgeMs': int(self.render_max_age*1000),
                'RenderPendingRequests': self.render_requests_pending,
                'RenderRequestHighWater': self.render_requests_high_water,
                'RenderRequests': self.render_request_count, 'RenderFragments': self.render_fragment_count,
                'IndependentAudioClock': self.independent_clock,
                'PreserveWirelessMode': self.preserve_wireless_mode,
                'UsbModeSwitchSuppressed': self.usb_mode_switch_suppressed,
                'CaptureConcealedFrames': self.capture_concealed,
                'RadioAudioMaxGapMs': round(self.radio_audio_max_gap*1000,3),
                'RenderCreditOverflow': self.render_credit_overflow,
                'CaptureDroppedFrames': self.capture_dropped, 'RenderExpiredRequests': self.render_dropped,
                'RenderDiscardedBufferedBytes': self.render_discarded_bytes,
                'RenderMaxQueueWaitMs': round(self.render_max_wait*1000, 3),
                'RadioAudioMaxAckMs': round(self.radio_ack_max_wait*1000, 3),
                'RadioBattery': self.battery}

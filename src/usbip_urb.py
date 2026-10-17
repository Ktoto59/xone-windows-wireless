# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded USB/IP URB transport with cancellable pluggable endpoint backends.

Wire format: kernel.org/doc/html/latest/usb/usbip_protocol.html.
"""
import asyncio
from dataclasses import dataclass
import struct
import time
import development_trace as trace
from usb_headset_profile import ControlState, Stall

MAX_TRANSFER = 65536
MAX_ISO_PACKETS = 256
MAX_PENDING = 64


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class IsoPacket:
    offset: int
    length: int
    actual: int = 0
    status: int = 0

    def pack(self):
        return struct.pack('!IIIi', self.offset, self.length, self.actual, self.status)


@dataclass(frozen=True)
class Submit:
    seq: int
    direction: int
    ep: int
    flags: int
    length: int
    frame: int
    packet_count: int
    interval: int
    setup: bytes
    data: bytes
    iso: tuple


@dataclass(frozen=True)
class Unlink:
    seq: int
    target: int


@dataclass(frozen=True)
class Completion:
    status: int = 0
    data: bytes = b''
    actual: int = 0
    iso: tuple = ()
    frame: int = 0


async def read_request(reader, timeout=5, devid=0x10001, on_header=None):
    async def take(length):
        return await asyncio.wait_for(reader.readexactly(length), timeout)

    header = await take(48)
    if on_header:
        on_header(header)
    command, seq, device, direction, ep = struct.unpack_from('!IIIII', header)
    if device != devid or direction not in (0, 1) or ep > 15:
        raise ProtocolError('Invalid USB/IP device, direction or endpoint')
    if command == 2:
        target = struct.unpack_from('!I', header, 20)[0]
        # usbip-win2 copies the original SUBMIT union into unused UNLINK
        # bytes (observed on native SET_CONFIGURATION cancellation). They are
        # padding, not another request; ignore them rather than disconnect.
        if ep != 0 or direction != 0:
            raise ProtocolError('Invalid UNLINK header')
        return Unlink(seq, target)
    if command != 1:
        raise ProtocolError('Unsupported USB/IP URB command')
    flags, length, frame, count, interval, setup = struct.unpack_from('!Iiiii8s', header, 20)
    if not 0 <= length <= MAX_TRANSFER or not -1 <= count <= MAX_ISO_PACKETS or interval < 0:
        raise ProtocolError('URB exceeds supported bounds')
    if ep == 0:
        if count > 0 or bool(setup[0] & 0x80) != bool(direction):
            raise ProtocolError('Invalid control direction or packet count')
        if struct.unpack_from('<H', setup, 6)[0] != length:
            raise ProtocolError('Control data length disagrees with setup')
    elif any(setup):
        raise ProtocolError('Setup packet on a non-control endpoint')
    data = await take(length) if direction == 0 and length else b''
    raw_iso = await take(count * 16) if count > 0 else b''
    iso = tuple(IsoPacket(*values) for values in struct.iter_unpack('!IIIi', raw_iso))
    last_end = 0
    for packet in iso:
        if packet.offset < last_end or packet.offset + packet.length > length:
            raise ProtocolError('Overlapping or out-of-buffer ISO packet')
        if packet.actual or packet.status:
            raise ProtocolError('ISO request contains completion fields')
        last_end = packet.offset + packet.length
    return Submit(seq, direction, ep, flags, length, frame, count, interval, setup, data, iso)


def pack_completion(request, result):
    if not 0 <= result.actual <= request.length:
        raise ProtocolError('Backend completion exceeds request buffer')
    if len(result.data) != (result.actual if request.direction else 0):
        raise ProtocolError('Backend completion payload length is inconsistent')
    errors = 0
    if len(result.iso) != len(request.iso):
        raise ProtocolError('Backend changed ISO packet count')
    for submitted, completed in zip(request.iso, result.iso):
        if (completed.offset, completed.length) != (submitted.offset, submitted.length) or completed.actual > submitted.length or completed.actual < 0:
            raise ProtocolError('Invalid backend ISO packet result')
        errors += completed.status != 0
    if request.iso and sum(p.actual for p in result.iso) != result.actual:
        raise ProtocolError('ISO actual lengths disagree with completion')
    # All server-response device/direction/endpoint fields are zero, per USB/IP.
    header = struct.pack('!IIIIIiiiii8x', 3, request.seq, 0, 0, 0,
                         result.status, result.actual, result.frame,
                         request.packet_count, errors)
    return header + result.data + b''.join(p.pack() for p in result.iso)


def failed_completion(request, status):
    return Completion(status=status, iso=tuple(IsoPacket(p.offset, p.length, status=status) for p in request.iso))


class UnavailableBackend:
    """Explicit failure; never acknowledges unsupported GIP or audio writes."""
    async def submit(self, request, endpoint):
        return failed_completion(request, -32)  # EPIPE / USB stall


class UrbSession:
    def __init__(self, profile, backend=None, timeout=5, on_event=None):
        self.control = ControlState(profile)
        self.backend = backend if backend is not None else UnavailableBackend()
        self.timeout = timeout
        self.on_event = on_event
        self.pending = {}
        self.writer = None

    def event(self, **fields):
        if self.on_event:
            self.on_event(fields)  # Headers/counters only, no PCM or GIP payload.

    def send_completion(self, request, result):
        packet = pack_completion(request, result)
        self.writer.write(packet)
        if self.on_event:
            self.event(operation='complete', seq=request.seq, ep=request.ep,
                       status=result.status, actual=result.actual, iso=len(result.iso))

    async def execute(self, request, endpoint):
        started = time.perf_counter_ns()
        trace.event('urb_backend_started', seq=request.seq, ep=endpoint.address, pending=len(self.pending))
        try:
            result = await self.backend.submit(request, endpoint)
            # Claim completion and write without an await: UNLINK cannot race in
            # between those two operations on this event loop.
            if self.pending.pop(request.seq, None) is None:
                return
            try:
                self.send_completion(request, result)
            except (ProtocolError, struct.error, TypeError, AttributeError):
                self.send_completion(request, failed_completion(request, -5))
            await asyncio.wait_for(self.writer.drain(), self.timeout)
        except asyncio.CancelledError:
            raise
        except (ConnectionError, TimeoutError):
            self.writer.close()
        except Exception as error:
            trace.event('urb_backend_exception', suspicious=True, seq=request.seq,
                        exception=type(error).__name__, pending=len(self.pending))
            if self.pending.pop(request.seq, None) is not None:
                self.send_completion(request, failed_completion(request, -5))
        finally:
            elapsed_us = (time.perf_counter_ns()-started)/1000
            trace.event('urb_backend_finished', seq=request.seq, ep=endpoint.address,
                        elapsed_us=elapsed_us, pending=len(self.pending))

    def cancel_disabled(self, reset=False):
        active = {e.address for e in self.control.active_endpoints()}
        for seq, (task, request) in list(self.pending.items()):
            address = request.ep | (0x80 if request.direction else 0)
            if reset or address not in active:
                self.pending.pop(seq)
                task.cancel()
                self.send_completion(request, failed_completion(request, -108))

    async def run(self, reader, writer):
        self.writer = writer
        tasks = set()
        try:
            while True:
                on_header = (lambda header: self.event(operation='urb_header', header=header.hex())) if self.on_event else None
                request = await read_request(reader, self.timeout, on_header=on_header)
                if self.on_event and isinstance(request, Submit):
                    self.event(operation='submit', seq=request.seq, ep=request.ep,
                               direction=request.direction, length=request.length,
                               setup=request.setup.hex() if request.ep == 0 else None,
                               iso=len(request.iso))
                if request.seq in self.pending:
                    raise ProtocolError('Duplicate active URB sequence')
                if isinstance(request, Unlink):
                    entry = self.pending.pop(request.target, None)
                    if entry is not None:
                        entry[0].cancel()
                    # Successful UNLINK replaces RET_SUBMIT entirely.
                    writer.write(struct.pack('!IIIIIi24x', 4, request.seq, 0, 0, 0, -104 if entry else 0))
                    self.event(operation='unlink', seq=request.seq, target=request.target, cancelled=entry is not None)
                elif request.ep == 0:
                    try:
                        data = self.control.request(request.setup, request.data)
                        self.send_completion(request, Completion(data=data, actual=len(data)))
                        if request.setup[:2] in (b'\x00\x09', b'\x01\x0b'):
                            self.cancel_disabled(reset=request.setup[:2] == b'\x00\x09')
                            if hasattr(self.backend, 'control_changed'):
                                self.backend.control_changed(request.setup)
                    except Stall:
                        self.send_completion(request, failed_completion(request, -32))
                else:
                    address = request.ep | (0x80 if request.direction else 0)
                    endpoint = next((e for e in self.control.active_endpoints() if e.address == address), None)
                    if endpoint is None:
                        self.send_completion(request, failed_completion(request, -32))
                    # Windows reserves e.g. 4096 bytes for a single ISO slot on
                    # the native GIP driver. USBip derives its length from buffer
                    # offsets, not an actual on-wire packet size. Buffer bounds
                    # are already checked by read_request; backend fills actual.
                    elif bool(request.iso) != (endpoint.transfer_type == 1):
                        self.send_completion(request, failed_completion(request, -22))
                    elif len(self.pending) >= MAX_PENDING:
                        trace.event('urb_queue_full', suspicious=True, seq=request.seq,
                                    ep=address, pending=len(self.pending), capacity=MAX_PENDING)
                        self.send_completion(request, failed_completion(request, -12))
                    else:
                        task = asyncio.create_task(self.execute(request, endpoint))
                        self.pending[request.seq] = (task, request)
                        tasks.add(task)
                        task.add_done_callback(tasks.discard)
                await asyncio.wait_for(writer.drain(), self.timeout)
        except (asyncio.IncompleteReadError, ConnectionError, TimeoutError, ProtocolError, ValueError) as error:
            self.event(operation='close', reason=type(error).__name__, detail=str(error))
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            self.pending.clear()
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded, lossy diagnostics on a dedicated I/O thread, never the RF loop."""
from collections import deque
import json
import threading
import development_trace as trace


class BridgeTelemetry:
    def __init__(self, status_file=None, *, emit=None, write_status=None, capacity=128):
        self.status_file = status_file
        self.emit = emit or (lambda fields: print(json.dumps(fields), flush=True))
        self.write_status = write_status or self._write_status
        self.capacity = capacity
        self.events = deque()
        self.snapshot = None
        self.condition = threading.Condition()
        self.stopping = False
        self.dropped = self.errors = 0
        self.event_count = self.diagnostic_count = 0
        self.previous_counters = {}
        self.thread = threading.Thread(target=self._run, name='USBIP telemetry', daemon=True)
        self.thread.start()

    def event(self, fields):
        if trace.active():
            operation = fields.get('operation', '')
            suspicious = (operation in ('close', 'radio_removed', 'radio_closed',
                            'automatic_worker_closed', 'automatic_stopped', 'attach_retry')
                          or any(word in operation for word in ('error', 'timeout', 'overflow', 'underrun'))
                          or (isinstance(fields.get('status'), (int, float)) and fields['status'] != 0))
            # The development file receives every event before the console cap.
            trace.event('bridge_event', suspicious=suspicious, event=fields)
        # Preserve the existing log limits; even a blocked console has a bound.
        self.event_count += 1
        diagnostic = fields.get('operation', '').startswith(('radio_', 'usb_audio_'))
        self.diagnostic_count += int(diagnostic)
        if not (self.event_count <= 1000 or (diagnostic and self.diagnostic_count <= 1000)
                or fields.get('operation') == 'close'):
            if self.event_count != 1001:
                return
            fields = {'operation': 'log_limit', 'limit': 1000}
        with self.condition:
            if len(self.events) >= self.capacity:
                self.events.popleft()
                self.dropped += 1
            self.events.append(fields)
            self.condition.notify()

    def publish(self, fields):
        if trace.active():
            increased = {}
            for key, value in fields.items():
                if isinstance(value, (int, float)) and not isinstance(value, bool) and any(
                        word in key.lower() for word in ('error', 'dropped', 'discarded', 'overflow', 'concealed', 'expired')):
                    previous = self.previous_counters.get(key, 0)
                    if value > previous:
                        increased[key] = dict(previous=previous, current=value)
                    self.previous_counters[key] = value
            for key, threshold in (('RadioAudioMaxGapMs', 32), ('RadioAudioMaxAckMs', 20),
                                   ('RenderMaxQueueWaitMs', fields.get('RenderMaxAgeMs', 64))):
                value = fields.get(key, 0)
                if isinstance(value, (int, float)):
                    if value > threshold and value > self.previous_counters.get(key, 0):
                        increased[key] = dict(previous=self.previous_counters.get(key, 0),
                                              current=value, threshold=threshold)
                    self.previous_counters[key] = value
            trace.event('status_snapshot', status=fields)
            if increased:
                trace.event('counter_increased', suspicious=True, counters=increased)
        if self.status_file is None:
            return
        with self.condition:
            # Latest state wins. Slow disks must never build a status backlog.
            self.snapshot = dict(fields, TelemetryDroppedEvents=self.dropped,
                                 TelemetryIoErrors=self.errors)
            self.condition.notify()

    def _write_status(self, fields):
        temporary = self.status_file.with_suffix('.tmp')
        temporary.write_text(json.dumps(fields), encoding='utf-8')
        temporary.replace(self.status_file)

    def _run(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.stopping or self.snapshot is not None or self.events)
                snapshot, self.snapshot = self.snapshot, None
                events = list(self.events)
                self.events.clear()
                stopping = self.stopping
            if snapshot is not None:
                try:
                    self.write_status(snapshot)
                except OSError:
                    self.errors += 1
                    trace.event('status_write_error', suspicious=True, errors=self.errors)
            for fields in events:
                try:
                    self.emit(fields)
                except OSError:
                    self.errors += 1
                    trace.event('console_write_error', suspicious=True, errors=self.errors)
            if stopping:
                return

    def close(self):
        with self.condition:
            self.stopping = True
            self.condition.notify()
        # A stalled external console/disk cannot hold process shutdown forever.
        self.thread.join(timeout=1)

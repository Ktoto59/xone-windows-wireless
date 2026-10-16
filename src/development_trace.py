# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in development tracing. No hardware, payload dumps or release dependency."""
import asyncio
from collections import deque
from datetime import datetime, timezone
import hashlib
import inspect
import json
import logging.handlers
import os
from pathlib import Path
import sys
import threading
import time

_active = None


def _metadata(value, depth=0):
    # Never call arbitrary repr(), traverse objects, or serialize PCM/key buffers.
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {'bytes': len(value)}
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, str):
        return value[:1024]
    if depth >= 4:
        return {'type': type(value).__name__}
    if isinstance(value, dict):
        return {str(k)[:80]: _metadata(v, depth+1) for k, v in list(value.items())[:96]}
    if isinstance(value, (list, tuple)):
        return [_metadata(v, depth+1) for v in value[:96]]
    return {'type': type(value).__name__}


class DevelopmentTrace:
    def __init__(self, directory, role, *, source=None, level='calls', capacity=8192,
                 max_bytes=16*1024*1024, backups=7, sink=None):
        if level not in ('calls', 'lines') or capacity < 1:
            raise ValueError('Invalid trace level or capacity')
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.role, self.level = role, level
        self.pid = os.getpid()
        self.session = self.directory.name
        self.capacity = capacity
        self.events = deque()
        self.anomalies = deque()
        self.condition = threading.Condition()
        self.sequence = self.dropped = self.io_errors = self.written = 0
        self.anomaly_dropped = 0
        self.stopping = False
        self.observed = set()
        self.source = Path(source or Path(__file__).parent).resolve()
        self.files = {str(p.resolve()): p.relative_to(self.source).as_posix()
                      for p in self.source.rglob('*.py') if p.resolve() != Path(__file__).resolve()}
        self.normalized_files = {os.path.normcase(path): name for path, name in self.files.items()}
        self.file_cache = dict(self.files)
        self.path = self.directory / f'{role}-{self.pid}.jsonl'
        self.health_path = self.directory / f'{role}-{self.pid}-health.json'
        self.handler = None
        self.anomaly_handler = logging.handlers.RotatingFileHandler(
            self.directory / f'anomalies-{role}-{self.pid}.jsonl',
            maxBytes=max_bytes, backupCount=backups, encoding='utf-8')
        self.anomaly_handler.handleError = self._handler_error
        if sink is None:
            self.handler = logging.handlers.RotatingFileHandler(
                self.path, maxBytes=max_bytes, backupCount=backups, encoding='utf-8')
            # Handler.emit normally swallows errors. This writer must count them.
            self.handler.handleError = self._handler_error
            sink = lambda row: self.handler.emit(logging.makeLogRecord(
                {'msg': json.dumps(row, ensure_ascii=True, allow_nan=False)}))
        self.sink = sink
        self.thread = threading.Thread(target=self._write, name='development trace', daemon=True)
        self.thread.start()
        self.previous_trace = self.previous_thread_trace = None
        self.installed = False
        self.event('trace_started', level=level, capacity=capacity, max_bytes=max_bytes,
                   backups=backups, python=sys.version, source_files=list(self.files.values()),
                   source_sha256={name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                  for path, name in self.files.items()})

    @staticmethod
    def _handler_error(record):
        raise OSError('Trace write failed')

    def event(self, operation, *, suspicious=False, **fields):
        row = {'operation': operation, 'utc': datetime.now(timezone.utc).isoformat(),
               'monotonic_ns': time.perf_counter_ns(), 'session': self.session,
               'pid': self.pid, 'thread': threading.get_ident(), 'role': self.role,
               'fields': _metadata(fields)}
        try:
            task = asyncio.current_task()
        except RuntimeError:
            task = None
        if task is not None:
            row['task'] = task.get_name()
            row['task_id'] = id(task)
        with self.condition:
            if self.stopping:
                return
            self.sequence += 1
            row['seq'] = self.sequence
            if suspicious:
                if len(self.anomalies) >= self.capacity:
                    self.anomaly_dropped += 1
                else:
                    self.anomalies.append(row)
                    self.condition.notify()
            if len(self.events) >= self.capacity:
                self.dropped += 1
                return
            self.events.append(row)
            self.condition.notify()

    def _trace(self, frame, event, arg):
        if self.stopping or threading.get_ident() == self.thread.ident:
            return None
        path = frame.f_code.co_filename
        if path not in self.file_cache:
            self.file_cache[path] = self.normalized_files.get(os.path.normcase(os.path.abspath(path)))
        filename = self.file_cache[path]
        if filename is None:
            return None
        frame.f_trace_lines = self.level == 'lines'
        if event in ('call', 'return', 'exception', 'line'):
            function = frame.f_code.co_qualname
            with self.condition:
                self.observed.add(f'{filename}:{function}')
            fields = dict(file=filename, function=function, line=frame.f_lineno, frame=id(frame))
            if event == 'exception':
                fields['exception'] = arg[0].__name__
                # Values can contain payloads; type/location identify the path.
            if frame.f_code.co_flags & (inspect.CO_COROUTINE | inspect.CO_GENERATOR | inspect.CO_ASYNC_GENERATOR):
                fields['resumable'] = True  # call/return also denote resume/yield.
            suspicious = (event == 'exception' and fields['exception'] not in
                          ('StopIteration', 'StopAsyncIteration', 'GeneratorExit', 'CancelledError'))
            self.event('python_'+event, suspicious=suspicious, **fields)
        return self._trace

    def install(self):
        if sys.gettrace() is not None or threading.gettrace() is not None:
            raise RuntimeError('Stop the Python debugger before enabling full development tracing')
        self.previous_trace, self.previous_thread_trace = sys.gettrace(), threading.gettrace()
        sys.settrace(self._trace)
        threading.settrace(self._trace)
        self.installed = True

    def _health(self):
        with self.condition:
            state = dict(Session=self.session, Pid=self.pid, Role=self.role, Queued=len(self.events),
                         Attempted=self.sequence, Written=self.written, Dropped=self.dropped,
                         IoErrors=self.io_errors, Stopping=self.stopping,
                         AnomalyDropped=self.anomaly_dropped,
                         ObservedFunctions=sorted(self.observed))
        temporary = self.health_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(state), encoding='utf-8')
        temporary.replace(self.health_path)

    def _write(self):
        previous_loss = (0, 0, 0)
        last_health = 0
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.events or self.anomalies or self.stopping, timeout=1)
                batch = list(self.events)
                self.events.clear()
                anomalies = list(self.anomalies)
                self.anomalies.clear()
                stopping = self.stopping
                dropped, errors = self.dropped, self.io_errors
                loss = (dropped, errors, self.anomaly_dropped)
                if loss != previous_loss:
                    self.sequence += 1
                    summary = dict(operation='trace_loss', utc=datetime.now(timezone.utc).isoformat(),
                                   monotonic_ns=time.perf_counter_ns(), session=self.session,
                                   pid=self.pid, thread=threading.get_ident(), role=self.role,
                                   seq=self.sequence, fields=dict(dropped=dropped, io_errors=errors,
                                                                 anomaly_dropped=self.anomaly_dropped))
                    batch.append(summary)
                    anomalies.append(summary)
                    previous_loss = loss
            for row in batch+anomalies:
                row['dropped_total'], row['io_errors_total'] = dropped, errors
            for row in anomalies:
                try:
                    self.anomaly_handler.emit(logging.makeLogRecord(
                        {'msg': json.dumps(row, ensure_ascii=True, allow_nan=False)}))
                except Exception:
                    self.io_errors += 1
            for row in batch:
                # Cumulative loss counters survive rotation and queue saturation.
                try:
                    self.sink(row)
                    self.written += 1
                except Exception:
                    self.io_errors += 1
            now = time.monotonic()
            if stopping or now-last_health >= 1:
                last_health = now
                try:
                    self._health()
                except Exception:
                    self.io_errors += 1
            if stopping:
                return

    def close(self):
        if self.installed:
            sys.settrace(self.previous_trace)
            threading.settrace(self.previous_thread_trace)
            self.installed = False
        self.event('trace_stopping', dropped=self.dropped, io_errors=self.io_errors)
        with self.condition:
            self.stopping = True
            self.condition.notify()
        self.thread.join(timeout=2)
        if not self.thread.is_alive() and self.handler is not None:
            self.handler.close()
        if not self.thread.is_alive():
            self.anomaly_handler.close()
        return not self.thread.is_alive()


def active():
    return _active is not None


def event(operation, **fields):
    if _active is not None:
        _active.event(operation, **fields)


def start_from_environment(role):
    global _active
    directory = os.environ.get('XONE_DEVELOPMENT_LOG_DIR')
    if not directory:
        return None
    if _active is not None:
        return _active
    trace = DevelopmentTrace(directory, role, level=os.environ.get('XONE_DEVELOPMENT_TRACE', 'calls'))
    try:
        trace.install()
    except BaseException:
        trace.close()
        raise
    _active = trace
    return trace


def stop():
    global _active
    trace, _active = _active, None
    if trace is not None:
        return trace.close()
    return True


async def monitor_loop():
    """Measure tracing/scheduling overhead and name asynchronous failures."""
    loop = asyncio.get_running_loop()
    previous = loop.get_exception_handler()
    def failed(loop, context):
        exception = context.get('exception')
        event('asyncio_unhandled', suspicious=True, message=context.get('message'),
              exception=type(exception).__name__ if exception is not None else None)
        if previous is not None:
            previous(loop, context)
        else:
            loop.default_exception_handler(context)
    loop.set_exception_handler(failed)
    try:
        while True:
            target = loop.time()+1
            await asyncio.sleep(1)
            lag_ms = max(0, (loop.time()-target)*1000)
            event('event_loop', suspicious=lag_ms > 20, lag_ms=lag_ms, tasks=len(asyncio.all_tasks()))
    finally:
        loop.set_exception_handler(previous)

# SPDX-License-Identifier: GPL-2.0-or-later
import asyncio
import json
from pathlib import Path
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'src'))
from development_trace import DevelopmentTrace
from bridge_telemetry import BridgeTelemetry
from usb_gip_radio import RadioBackend


class DevelopmentTraceTests(unittest.TestCase):
    def rows(self, directory, pattern):
        return [json.loads(line) for path in Path(directory).glob(pattern)
                for line in path.read_text(encoding='utf-8').splitlines()]

    def test_all_source_calls_threads_coroutines_and_caught_errors_without_values(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'source'
            source.mkdir()
            path = source/'fixture.py'
            path.write_text('''import asyncio
def child(secret):
    try:
        raise ValueError(secret)
    except ValueError:
        return 42
async def coroutine(secret):
    await asyncio.sleep(0)
    return child(secret)
''', encoding='utf-8')
            namespace = {}
            exec(compile(path.read_text(), str(path.resolve()), 'exec'), namespace)
            rows = []
            trace = DevelopmentTrace(directory, 'test', source=source, level='lines', sink=rows.append)
            try:
                trace.install()
                self.assertEqual(asyncio.run(namespace['coroutine']('private-pcm-key')), 42)
                thread = threading.Thread(target=namespace['child'], args=('private-pcm-key',))
                thread.start()
                thread.join(2)
                self.assertFalse(thread.is_alive())
            finally:
                self.assertTrue(trace.close())
            calls = [r for r in rows if r['operation'] == 'python_call']
            self.assertEqual({r['fields']['function'] for r in calls}, {'child', 'coroutine'})
            self.assertGreaterEqual(len({r['thread'] for r in calls}), 2)
            self.assertTrue(any(r.get('task') for r in calls))
            self.assertTrue(any(r['operation'] == 'python_line' for r in rows))
            errors = self.rows(directory, 'anomalies-*.jsonl')
            self.assertTrue(any(r['fields'].get('exception') == 'ValueError' for r in errors))
            self.assertNotIn('private-pcm-key', json.dumps(rows+errors))

    def test_blocked_writer_bounds_memory_and_preserves_separate_anomaly_queue(self):
        entered, release = threading.Event(), threading.Event()
        rows = []
        def sink(row):
            entered.set()
            release.wait(2)
            rows.append(row)
        with tempfile.TemporaryDirectory() as directory:
            trace = DevelopmentTrace(directory, 'test', capacity=2, sink=sink)
            try:
                self.assertTrue(entered.wait(1))
                for number in range(100):
                    trace.event('ordinary', number=number)
                trace.event('fatal_usb', suspicious=True, error=31)
                self.assertLessEqual(len(trace.events), 2)
                self.assertEqual(len(trace.anomalies), 1)
                self.assertGreater(trace.dropped, 0)
            finally:
                release.set()
                self.assertTrue(trace.close())
            errors = self.rows(directory, 'anomalies-*.jsonl')
            self.assertTrue(any(r['operation'] == 'fatal_usb' for r in errors))
            self.assertTrue(any(r['operation'] == 'trace_loss' for r in errors))
            health = json.loads(trace.health_path.read_text())
            self.assertEqual(health['Dropped'], trace.dropped)
            self.assertEqual(health['AnomalyDropped'], 0)

    def test_disk_failure_is_counted_and_does_not_escape_to_radio(self):
        def fail(row):
            raise PermissionError('test disk failure')
        with tempfile.TemporaryDirectory() as directory:
            trace = DevelopmentTrace(directory, 'test', sink=fail)
            trace.event('after_failure')
            self.assertTrue(trace.close())
            self.assertGreater(trace.io_errors, 0)
            self.assertGreater(json.loads(trace.health_path.read_text())['IoErrors'], 0)

    def test_rotating_logs_and_metadata_only_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = DevelopmentTrace(directory, 'test', max_bytes=512, backups=2)
            for i in range(40):
                trace.event('packet', suspicious=True, number=i, buffer=b'private audio')
            self.assertTrue(trace.close())
            self.assertLessEqual(len(list(Path(directory).glob('test-*.jsonl*'))), 3)
            self.assertLessEqual(len(list(Path(directory).glob('anomalies-*.jsonl*'))), 3)
            for path in Path(directory).glob('*.jsonl*'):
                text = path.read_text(encoding='utf-8')
                self.assertNotIn('private audio', text)
                for line in text.splitlines():
                    row = json.loads(line)
                    self.assertEqual(row['session'], Path(directory).name)

    def test_development_events_continue_after_console_limit_and_anomalies_have_same_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = []
            trace = DevelopmentTrace(directory, 'test', capacity=4096, sink=rows.append)
            with patch('development_trace._active', trace):
                telemetry = BridgeTelemetry(emit=lambda fields: None)
                try:
                    for i in range(1205):
                        telemetry.event({'operation':'submit', 'seq':i})
                    telemetry.event({'operation':'complete', 'seq':1205, 'status':-5})
                    telemetry.publish({'CaptureDroppedFrames':1})
                    telemetry.publish({'CaptureDroppedFrames':1})
                finally:
                    telemetry.close()
            self.assertTrue(trace.close())
            events = [r for r in rows if r['operation'] == 'bridge_event']
            self.assertEqual(len(events), 1206)
            anomalies = self.rows(directory, 'anomalies-*.jsonl')
            failed = next(r for r in anomalies if r['operation'] == 'bridge_event')
            self.assertEqual(failed['seq'], events[-1]['seq'])
            self.assertEqual(len([r for r in anomalies if r['operation'] == 'counter_increased']), 1)

    def test_each_audio_drop_and_expiration_is_logged_before_any_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = []
            trace = DevelopmentTrace(directory, 'test', sink=rows.append)
            backend = RadioBackend('020000000001', audio_bridge=True, clock=lambda: 1)
            with patch('development_trace._active', trace):
                backend.audio_queue.extend((0, b'private audio') for _ in range(5))
                backend.trim_capture()
                backend.render_pcm.extend(b'private audio')
                backend.discard_render(SimpleNamespace(seq=7, iso=(), frame=0))
            self.assertTrue(trace.close())
            anomalies = self.rows(directory, 'anomalies-*.jsonl')
            self.assertEqual(len([r for r in anomalies if r['operation']=='capture_frame_dropped']), 5)
            self.assertEqual(len([r for r in anomalies if r['operation']=='render_expired']), 1)
            self.assertNotIn('private audio', json.dumps(rows+anomalies))


if __name__ == '__main__':
    unittest.main()

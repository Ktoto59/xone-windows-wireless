import asyncio
from pathlib import Path
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from bridge_telemetry import BridgeTelemetry


class TelemetryTests(unittest.IsolatedAsyncioTestCase):
    async def test_blocked_disk_does_not_block_loop_and_status_stays_latest(self):
        entered, release = threading.Event(), threading.Event()
        written, logs = [], []
        def write(fields):
            entered.set()
            release.wait(2)
            written.append(fields)
        worker = BridgeTelemetry(Path('unused.json'), emit=logs.append, write_status=write, capacity=4)
        try:
            worker.publish({'Generation': 0})
            self.assertTrue(await asyncio.to_thread(entered.wait, 1))
            # The worker is blocked; publishers must still run on the loop.
            async def network_work():
                for i in range(100):
                    worker.publish({'Generation': i+1})
                    worker.event({'operation': 'radio_gip_in', 'number': i})
                await asyncio.sleep(0)
                return 'control completed'
            self.assertEqual(await asyncio.wait_for(network_work(), .5), 'control completed')
            self.assertLessEqual(len(worker.events), 4)
            self.assertEqual(worker.dropped, 96)
        finally:
            release.set()
            await asyncio.to_thread(worker.close)
        self.assertEqual(written[-1]['Generation'], 100)
        self.assertLessEqual(len(written), 2)
        self.assertEqual(logs[-1]['number'], 99)

    async def test_failed_status_and_console_writes_are_not_radio_errors(self):
        def fail(_):
            raise PermissionError('simulated locked status/console')
        worker = BridgeTelemetry(Path('unused.json'), emit=fail, write_status=fail)
        worker.event({'operation': 'radio_associated'})
        worker.publish({'Ready': True})
        await asyncio.to_thread(worker.close)
        self.assertEqual(worker.errors, 2)


if __name__ == '__main__':
    unittest.main()

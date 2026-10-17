# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise installed state/query/stop tools without opening a physical adapter."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RuntimeStateIntegrationTests(unittest.TestCase):
    def test_query_and_stop_use_memory_despite_stale_files(self):
        candidate = Path(os.environ.get('XONE_TEST_CANDIDATE', ROOT/'outputs/appliance-candidate'))
        if not (candidate/'runtime-state-tests.exe').exists():
            self.skipTest('Build-Appliance.cmd must complete first')
        with tempfile.TemporaryDirectory(dir=ROOT/'work', prefix='memory-ipc-') as directory:
            root = Path(directory)
            output = root/'outputs'
            output.mkdir()
            (root/'work').mkdir()
            for name in ('xone-background.exe', 'runtime-capabilities.json'):
                shutil.copy2(candidate/name, output/name)
            stale = root/'work/maintenance-status.json'
            stale.write_text('{"Phase":"Stale","Process":{"Pid":0}}', encoding='utf-8')
            script = root/'query.ps1'
            script.write_text('param($Helpers,$Root)\n. $Helpers\n'
                              'Read-XoneJson (Join-Path $Root "work/maintenance-status.json") | '
                              'ConvertTo-Json -Depth 8\n', encoding='utf-8-sig')
            before = {p.relative_to(root): p.stat().st_mtime_ns for p in root.rglob('*') if p.is_file()}
            server = subprocess.Popen([str(candidate/'runtime-state-tests.exe'), '--serve', str(root)],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertEqual(server.stdout.readline().strip(), 'READY')
                state = json.loads(subprocess.check_output([str(output/'xone-background.exe'), '--status'], text=True))
                self.assertEqual(state['Host']['Phase'], 'Testing')
                self.assertEqual(state['Host']['Process']['Pid'], server.pid)
                powershell = Path(os.environ['SystemRoot'])/'System32/WindowsPowerShell/v1.0/powershell.exe'
                result = subprocess.check_output([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                                  '-File', str(script), str(ROOT/'scripts/Setup-Helpers.ps1'),
                                                  str(root)], text=True, encoding='utf-8')
                self.assertEqual(json.loads(result)['Phase'], 'Testing')
                subprocess.run([str(output/'xone-background.exe'), '--stop'], check=True, timeout=10)
                server.communicate(timeout=5)
                self.assertEqual(server.returncode, 0)
                self.assertEqual(json.loads(subprocess.check_output([str(output/'xone-background.exe'), '--status'], text=True)), {})
                after = {p.relative_to(root): p.stat().st_mtime_ns for p in root.rglob('*') if p.is_file()}
                self.assertEqual(before, after, 'Runtime query/stop wrote application files')
            finally:
                if server.poll() is None:
                    server.kill()
                    server.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()

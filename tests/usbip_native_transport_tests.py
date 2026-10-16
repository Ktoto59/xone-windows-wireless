# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the C++ event-driven server over real loopback sockets, without USB."""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import unittest
import json
import tempfile
import time
import os
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_headset_profile import HeadsetProfile


def exact(sock, count):
    data = b''
    while len(data) < count:
        part = sock.recv(count-len(data))
        if not part:
            raise EOFError('C++ USB/IP peer closed early')
        data += part
    return data


def submit(seq, direction, endpoint, data=b'', length=None, setup=bytes(8), iso=()):
    if length is None:
        length = len(data)
    return struct.pack('!IIIIIIiiii8s', 1, seq, 0x10001, direction, endpoint, 0,
                       length, 0, len(iso) if iso else -1, 1, setup) + data + b''.join(
                           struct.pack('!IIII', offset, count, 0, 0) for offset, count in iso)


class NativeTransportTests(unittest.TestCase):
    def start(self, pid, status=None):
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            self.port = reserve.getsockname()[1]
        command = [str(ROOT/'outputs/usbip-native-server-probe.exe'), str(self.port), pid]
        if status is not None:
            command.append(str(status))
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop)
        self.assertEqual(self.process.stdout.readline().strip(), 'READY')

    def stop(self):
        if self.process.poll() is None:
            self.process.stdin.write('quit\n')
            self.process.stdin.flush()
        try:
            out, err = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
            raise AssertionError('Native server did not stop')
        self.assertEqual(self.process.returncode, 0, err)

    def connect(self, operation, bus=b'1-1'):
        sock = socket.create_connection(('127.0.0.1', self.port), timeout=3)
        self.addCleanup(sock.close)
        wire = struct.pack('!HHI', 0x0111, operation, 0)
        if operation == 0x8003:
            wire += bus.ljust(32, b'\0')
        # Real partial framing, including a split USB/IP handshake.
        sock.sendall(wire[:3])
        sock.sendall(wire[3:])
        return sock

    def test_all_profiles_match_python_and_exclusive_import(self):
        for pid, name in [('0b32', 'headset'), ('0b17', 'headset'), ('0b12', 'controller')]:
            with self.subTest(pid=pid):
                self.start(pid)
                reference = HeadsetProfile.load(ROOT/f'driver/profiles/{name}-{pid}-usb.json')
                if name == 'headset':
                    reference.usbip_speed = 3
                sock = self.connect(0x8005)
                self.assertEqual(exact(sock, len(reference.usbip_device_list())), reference.usbip_device_list())
                sock.close()
                bad = self.connect(0x8003, b'2-2')
                self.assertEqual(exact(bad, 8), bytes.fromhex('0111000300000001'))
                owner = self.connect(0x8003)
                self.assertEqual(exact(owner, 320), bytes.fromhex('0111000300000000') + reference.usbip_device())
                other = self.connect(0x8003)
                self.assertEqual(exact(other, 8), bytes.fromhex('0111000300000001'))
                owner.sendall(submit(1, 1, 0, length=18, setup=bytes.fromhex('8006000100001200')))
                self.assertEqual(struct.unpack_from('!i', exact(owner, 48), 20)[0], 0)
                self.assertEqual(exact(owner, 18), reference.device)
                owner.close()
                self.stop()
                # stop was also registered as cleanup: already exited is harmless.

    def test_audio_and_unlink_use_actual_tcp_replies(self):
        self.start('0b32')
        sock = self.connect(0x8003)
        exact(sock, 320)
        for seq, setup in [(1, '0009010000000000'), (2, '010b010001000000')]:
            sock.sendall(submit(seq, 0, 0, setup=bytes.fromhex(setup)))
            self.assertEqual(struct.unpack_from('!i', exact(sock, 48), 20)[0], 0)
        sock.sendall(submit(3, 1, 1, length=64))
        sock.sendall(struct.pack('!IIIIII24x', 2, 4, 0x10001, 0, 0, 3))
        self.assertEqual(struct.unpack_from('!IIIIIi', exact(sock, 48)), (4, 4, 0, 0, 0, -104))
        sock.sendall(submit(5, 1, 2, length=2048, iso=tuple((n*256, 256) for n in range(8))))
        self.process.stdin.write('audio\n')
        self.process.stdin.flush()
        header = exact(sock, 48)
        self.assertEqual(struct.unpack_from('!iii', header, 20), (0, 432, 0))
        payload, slots = exact(sock, 432), exact(sock, 128)
        self.assertEqual(payload[:8], bytes((0x60, 32, 1, 50, 192, 0, 2, 3)))
        self.assertEqual([struct.unpack_from('!IIII', slots, n*16)[2] for n in range(8)], [54]*8)

    def test_mmcss_can_be_disabled_without_disabling_usb(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        status = Path(directory.name)/'status.json'
        with patch.dict(os.environ, {'XONE_USB_MMCSS': '0'}):
            self.start('0b32', status)
        owner = self.connect(0x8003)
        exact(owner, 320)
        owner.sendall(submit(1, 0, 0, setup=bytes.fromhex('0009010000000000')))
        self.assertEqual(struct.unpack_from('!i', exact(owner, 48), 20)[0], 0)
        owner.sendall(submit(2, 1, 1, length=64))
        self.process.stdin.write('input\n')
        self.process.stdin.flush()
        self.assertEqual(struct.unpack_from('!ii', exact(owner, 48), 20), (0, 8))
        self.assertEqual(exact(owner, 8), bytes((3, 32, 1, 4, 0, 0, 0, 0)))
        deadline = time.monotonic()+4
        while time.monotonic() < deadline:
            try:
                state = json.loads(status.read_text(encoding='utf-8'))
                if state['UsbInitialization']['InterruptDelivered'] == 1:
                    break
            except (OSError, ValueError):
                pass
            time.sleep(.02)
        else:
            self.fail('Initialization snapshot timeout')
        self.assertFalse(state['MmcssRequested'])
        self.assertFalse(state['Mmcss'])
        self.assertEqual(state['MmcssError'], 0)
        self.assertEqual(state['UsbInitialization']['Configurations'], 1)
        self.assertEqual(state['UsbInitialization']['InterruptIn'], 1)
        self.assertGreater(state['WorkerIterations'], 0)
        self.assertGreaterEqual(state['WorkerCpu100ns'], 0)

    def test_import_and_close_status_bypass_counter_cadence(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        status = Path(directory.name)/'status.json'
        self.start('0b32', status)

        def wait_for(predicate):
            # Shorter than the two-second counter interval, while allowing
            # the asynchronous file worker ample time on Windows CI.
            deadline = time.monotonic()+1.5
            while time.monotonic() < deadline:
                try:
                    snapshot = json.loads(status.read_text(encoding='utf-8'))
                    if predicate(snapshot):
                        return snapshot
                except (OSError, ValueError):
                    pass
                time.sleep(.01)
            self.fail('Lifecycle status waited for the counter interval')

        wait_for(lambda value: value['Ready'] and not value['UsbImported'])
        owner = self.connect(0x8003)
        exact(owner, 320)
        wait_for(lambda value: value['UsbImported'])
        owner.close()
        wait_for(lambda value: not value['UsbImported'] and value['UsbClosed'] == 1)
        self.stop()

    def test_audio_error_reimports_without_new_rf_and_is_journaled(self):
        directory=tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        directory=directory.name
        status=Path(directory)/'status.json'
        with patch.dict(os.environ,{'XONE_CPP_LOG_DIR':directory}):
            self.start('0b32',status)
        def wait_for(predicate):
            deadline=time.monotonic()+4
            while time.monotonic()<deadline:
                try:
                    state=json.loads(status.read_text(encoding='utf-8'))
                    if predicate(state):return state
                except (OSError,ValueError):pass
                time.sleep(.01)
            self.fail('Recovery snapshot timeout')
        def configure(sock):
            for seq,setup in ((1,'0009010000000000'),(2,'010b010001000000')):
                sock.sendall(submit(seq,0,0,setup=bytes.fromhex(setup)))
                self.assertEqual(struct.unpack_from('!i',exact(sock,48),20)[0],0)
        def audio():
            self.process.stdin.write('audio\n');self.process.stdin.flush()
            wait_for(lambda s:s.get('AudioLayoutSupported'))
            self.process.stdin.write('audio\n');self.process.stdin.flush()
        def render(sock,seq):
            block=bytes((0x60,32,1,0xc0,0x81,0))+bytes((77,))*192
            sock.sendall(submit(seq,0,2,data=block*8,iso=tuple((n*198,198) for n in range(8))))
            header=exact(sock,48);exact(sock,128)
            return struct.unpack_from('!ii',header,20)
        owner=self.connect(0x8003);exact(owner,320);configure(owner)
        initial=wait_for(lambda s:s['Ready'] and s['UsbImported'])
        self.process.stdin.write('fail-next\n');self.process.stdin.flush()
        self.assertEqual(self.process.stdout.readline().strip(),'ARMED')
        audio()
        self.assertEqual(render(owner,10),(-5,0))
        failed=wait_for(lambda s:s.get('RecoveryRequired') and not s['Ready'])
        self.assertTrue(failed['RadioAssociated']);self.assertNotEqual(initial['Generation'],failed['Generation'])
        owner.close();wait_for(lambda s:not s['UsbImported'])
        restored=self.connect(0x8003)
        self.assertEqual(exact(restored,320)[:8],bytes.fromhex('0111000300000000'))
        configure(restored);audio()
        self.assertEqual(render(restored,11),(0,1584))
        wait_for(lambda s:s['Ready'] and s['UsbImported'] and not s['RecoveryRequired'])
        restored.close();self.stop()
        rows=[json.loads(line) for path in Path(directory).glob('anomalies-*.jsonl')
              for line in path.read_text(encoding='utf-8').splitlines()]
        self.assertTrue(any(r['operation']=='audio_recovery_required' for r in rows))
        self.assertTrue(any(r['operation']=='radio_tx_complete' and r['fields']['status']==-5 for r in rows))


if __name__ == '__main__':
    unittest.main()

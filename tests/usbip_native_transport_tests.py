# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the C++ event-driven server over real loopback sockets, without USB."""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import unittest

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
    def start(self, pid):
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            self.port = reserve.getsockname()[1]
        self.process = subprocess.Popen([str(ROOT/'outputs/usbip-native-server-probe.exe'),
                                         str(self.port), pid], stdin=subprocess.PIPE,
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


if __name__ == '__main__':
    unittest.main()

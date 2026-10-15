# SPDX-License-Identifier: GPL-2.0-or-later
"""Production automatic routes, simultaneous imports and slot reuse over TCP."""
from pathlib import Path
import socket
import struct
import subprocess
import unittest
from usbip_native_transport_tests import exact, submit

ROOT = Path(__file__).resolve().parents[1]


class MultiRouteTests(unittest.TestCase):
    def test_two_revisions_same_model_instances_and_reassociation(self):
        # Reserve four non-overlapping loopback ports before starting the helper.
        reservations = []
        try:
            for base in range(35000, 60000, 10):
                try:
                    for n in range(4):
                        s = socket.socket()
                        reservations.append(s)
                        s.bind(('127.0.0.1', base + 2*n))
                    break
                except OSError:
                    for s in reservations:
                        s.close()
                    reservations.clear()
            else:
                self.fail('No free test port range')
        finally:
            for s in reservations:
                s.close()
        process = subprocess.Popen([str(ROOT/'outputs/native-multi-route-probe.exe'), str(base)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        sockets = []
        try:
            self.assertEqual(process.stdout.readline().strip(), 'READY')

            def connect(n):
                s = socket.create_connection(('127.0.0.1', base + 2*n), timeout=3)
                sockets.append(s)
                s.sendall(struct.pack('!HHI', 0x111, 0x8003, 0) + b'1-1'.ljust(32, b'\0'))
                self.assertEqual(exact(s, 8), bytes.fromhex('0111000300000000'))
                metadata = exact(s, 312)
                return s, struct.unpack_from('!H', metadata, 302)[0]

            def serial(s, seq=1):
                s.sendall(submit(seq*2, 1, 0, length=18, setup=bytes.fromhex('8006000100001200')))
                header = exact(s, 48)
                self.assertEqual(struct.unpack_from('!i', header, 20)[0], 0)
                device = exact(s, 18)
                setup = struct.pack('<BBHHH', 0x80, 6, 0x300 | device[16], 0x409, 255)
                s.sendall(submit(seq*2+1, 1, 0, length=255, setup=setup))
                header = exact(s, 48)
                self.assertEqual(struct.unpack_from('!i', header, 20)[0], 0)
                return exact(s, struct.unpack_from('!I', header, 24)[0])[2:].decode('utf-16-le')

            peers = [connect(n) for n in range(4)]
            self.assertEqual([pid for _, pid in peers], [0xb17, 0xb32, 0xb12, 0xb32])
            identities = [serial(s) for s, _ in peers]
            self.assertEqual(len(set(identities)), 4)
            self.assertEqual(identities[0], 'XONE0B17020000000001')

            def command(value):
                process.stdin.write(value+'\n')
                process.stdin.flush()
                self.assertEqual(process.stdout.readline().strip(), 'OK')

            command('remove-old')
            self.assertEqual(peers[0][0].recv(1), b'')
            for n in (1, 2, 3):
                self.assertEqual(serial(peers[n][0], 2), identities[n])
            command('add-old')
            replacement, pid = connect(0)
            self.assertEqual(pid, 0xb17)
            self.assertEqual(serial(replacement), identities[0])
            for n in (1, 2, 3):
                self.assertEqual(serial(peers[n][0], 3), identities[n])
        finally:
            for s in sockets:
                s.close()
            if process.poll() is None:
                process.stdin.write('quit\n')
                process.stdin.flush()
            try:
                _, err = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                self.fail('Multi-route helper did not stop')
            self.assertEqual(process.returncode, 0, err)


if __name__ == '__main__':
    unittest.main()

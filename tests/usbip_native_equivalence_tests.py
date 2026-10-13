# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare native USB metadata/control bytes with the existing Python reference."""
from pathlib import Path
import struct
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'src'))
from usb_headset_profile import HeadsetProfile, ControlState
from usbip_reference_server import transport_profile

class NativeEquivalence(unittest.TestCase):
    def test_public_profiles_match_reference_byte_for_byte(self):
        binary=ROOT/'outputs/usbip-native-codec-tests.exe'
        if not binary.exists():
            self.skipTest('Build native codec tests first')
        for name in ('headset-0b32-usb.json','headset-0b17-usb.json','controller-0b12-usb.json'):
            for audio in (False,True):
                with self.subTest(name=name,audio=audio):
                    p=transport_profile(ROOT/'driver/profiles'/name,audio)
                    run=subprocess.run([str(binary),'dump',f'{p.pid:04x}','audio' if audio else 'plain'],check=True,capture_output=True,text=True)
                    values={key:bytes.fromhex(value) for key,value in (line.split('\t') for line in run.stdout.splitlines())}
                    self.assertEqual(values['metadata'],p.usbip_device())
                    self.assertEqual(values['list'],p.usbip_device_list())
                    control=ControlState(p)
                    queries=((0x80,6,0x0100,0),(0x80,6,0x0200,0),(0x80,6,0x03ee,0),(0xc0,p.os_vendor_code,0,4),
                             *((0x80,6,0x0300|index,0x0409) for index in p.device[14:17]))
                    for i,(kind,request,value,index) in enumerate(queries):
                        self.assertEqual(values[f'control{i}'],control.request(struct.pack('<BBHHH',kind,request,value,index,255)))

if __name__=='__main__':unittest.main()

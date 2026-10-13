import copy
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from usb_headset_profile import HeadsetProfile, ControlState, Stall
from usbip_reference_server import radio_endpoints


def setup(kind, request, value=0, index=0, length=0):
    return struct.pack('<BBHHH', kind, request, value, index, length)


class WiredReferenceTests(unittest.TestCase):
    def setUp(self):
        self.document = json.loads((ROOT / 'driver/profiles/headset-0b32-usb.json').read_text())
        self.profile = HeadsetProfile(self.document)
        self.control = ControlState(self.profile)

    def test_real_descriptors_and_speed_mapping(self):
        self.assertEqual(self.profile.usbip_speed, 2)  # full speed, not low speed
        self.assertEqual(len(self.profile.configuration), 64)
        self.assertEqual(self.profile.classes, {0: (255, 71, 208), 1: (255, 71, 208)})
        self.assertEqual([(e.address, e.transfer_type, e.max_packet, e.interval)
                          for e in self.profile.settings[(1, 1)]], [(2, 1, 228, 1), (130, 1, 64, 1)])

    def test_captured_old_headset_has_own_channels_vendor_code_and_identity(self):
        profile = HeadsetProfile.load(ROOT / 'driver/profiles/headset-0b17-usb.json')
        self.assertEqual((profile.pid,profile.revision,profile.os_vendor_code),(0x0b17,0x0100,0xee))
        self.assertEqual(radio_endpoints(profile),((0x0a,0x86),(0x01,0x82)))
        self.assertEqual(radio_endpoints(self.profile),((0x01,0x81),(0x02,0x82)))
        control = ControlState(profile)
        serial = control.request(setup(0x80,6,0x0304,0x0409,255))[2:].decode('utf-16le')
        self.assertEqual(serial,'XONE0B17TEST')
        self.assertNotEqual(serial,self.profile.virtual_serial)
        self.assertEqual(control.request(setup(0xc0,0xee,index=4,length=255))[18:26],b'XGIP10\0\0')

    def test_audio_endpoints_exist_only_after_configuration_and_alternate(self):
        self.assertEqual(self.control.active_endpoints(), [])
        with self.assertRaises(Stall):
            self.control.request(setup(1, 11, 1, 1))
        self.control.request(setup(0, 9, 1))
        self.assertEqual([e.address for e in self.control.active_endpoints()], [1, 129])
        self.control.request(setup(1, 11, 1, 1))
        self.assertEqual([e.address for e in self.control.active_endpoints()], [1, 129, 2, 130])
        self.control.request(setup(0, 9, 0))
        self.assertEqual(self.control.active_endpoints(), [])

    def test_series_captured_identity_and_endpoint_alternates(self):
        profile = HeadsetProfile.load(ROOT / 'driver/profiles/controller-0b12-usb.json')
        self.assertEqual((profile.pid,profile.revision,profile.os_vendor_code),(0x0b12,0x0518,0x90))
        self.assertEqual(radio_endpoints(profile),((2,0x82),(3,0x83)))
        control = ControlState(profile)
        self.assertEqual(control.request(setup(0x80,6,0x0302,0x0409,255))[2:].decode('utf-16le'),'Xbox Controller')
        self.assertEqual(control.request(setup(0x80,6,0x0303,0x0409,255))[2:].decode('utf-16le'),'XONE0B12TEST')
        control.request(setup(0,9,1))
        self.assertEqual([(e.address,e.interval) for e in control.active_endpoints()],[(2,4),(0x82,4)])
        control.request(setup(1,11,1,0))
        self.assertEqual([(e.address,e.interval) for e in control.active_endpoints()],[(2,4),(0x82,2)])
        control.request(setup(1,11,1,2))
        self.assertEqual([e.address for e in control.active_endpoints()],[2,0x82,4,0x84])

    def test_failed_configuration_does_not_mutate_state(self):
        self.control.request(setup(0, 9, 1))
        with self.assertRaises(Stall):
            self.control.request(setup(0, 9, 2))
        with self.assertRaises(Stall):
            self.control.request(setup(1, 11, 2, 1))
        self.assertEqual(self.control.configured, 1)
        self.assertEqual(self.control.alternate, {0: 0, 1: 0})

    def test_usbip_wire_metadata(self):
        packet = self.profile.usbip_device_list()
        self.assertEqual(packet[:12], bytes.fromhex('011100050000000000000001'))
        self.assertEqual(len(packet), 332)
        self.assertEqual(struct.unpack_from('!IIIHHH6B', packet, 300),
                         (1, 1, 2, 0x045e, 0x0b32, 0x3114, 255, 71, 208, 1, 1, 2))
        self.assertEqual(packet[-8:], bytes.fromhex('ff47d000ff47d000'))

    def test_descriptor_length_probe_is_valid(self):
        self.assertEqual(self.control.request(setup(0x80, 6, 0x0200, length=9)), self.profile.configuration[:9])
        self.assertEqual(self.control.request(setup(0x80, 6, 0x0100, length=100)), self.profile.device)

    def test_virtual_serial_does_not_clone_physical_serial(self):
        descriptor = self.control.request(setup(0x80, 6, 0x0307, 0x0409, 255))
        self.assertEqual(descriptor[2:].decode('utf-16le'), 'XONE0B32TEST')

    def test_xgip10_compatible_id(self):
        os_string = self.control.request(setup(0x80, 6, 0x03ee, length=255))
        self.assertEqual(os_string, bytes.fromhex('14034d0053004600540031003000300090000390'))
        self.assertEqual(self.profile.os_vendor_code, 0x90)
        compatible = self.control.request(setup(0xc0, 0x90, index=4, length=255))
        self.assertEqual(len(compatible), 40)
        self.assertEqual(compatible[:10], bytes.fromhex('28000000000104000100'))
        self.assertEqual(compatible[18:26], b'XGIP10\0\0')

    def test_unknown_vendor_and_firmware_commands_stall(self):
        for kind, request, index in [(0x40, 0x90, 0), (0xc0, 0x90, 5), (0xc0, 0x20, 4), (0x21, 9, 0)]:
            with self.subTest(kind=kind, request=request, index=index), self.assertRaises(Stall):
                self.control.request(setup(kind, request, index=index))

    def test_truncated_control_and_wrong_data_stage_rejected(self):
        for packet, payload in [(b'\x80\x06', b''), (setup(0x40, 1, length=4), b'a'),
                                (setup(0x80, 6, 0x0100, length=18), b'x')]:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                self.control.request(packet, payload)

    def test_malformed_captured_profiles_rejected(self):
        for position, replacement in [(2, 63), (4, 3), (9, 0), (13, 1), (57, 30)]:
            document = copy.deepcopy(self.document)
            data = bytearray.fromhex(document['ConfigurationDescriptor'])
            data[position] = replacement
            document['ConfigurationDescriptor'] = data.hex()
            with self.subTest(position=position), self.assertRaises(ValueError):
                HeadsetProfile(document)


if __name__ == '__main__':
    unittest.main()

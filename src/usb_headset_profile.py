# SPDX-License-Identifier: GPL-2.0-or-later
"""Descriptor/control foundation for a native wired GIP facade.

No USB access, firmware forwarding, radio, audio generation, or driver install.
USB/IP serialization follows kernel.org/doc/html/latest/usb/usbip_protocol.html.
"""
from dataclasses import dataclass
from pathlib import Path
import json
import struct


class Stall(ValueError):
    """The emulated device must stall an unsupported USB control request."""


@dataclass(frozen=True)
class Endpoint:
    address: int
    transfer_type: int
    max_packet: int
    interval: int


class HeadsetProfile:
    def __init__(self, document):
        self.device = bytes.fromhex(document['DeviceDescriptor'])
        self.configuration = bytes.fromhex(document['ConfigurationDescriptor'])
        d, c = self.device, self.configuration
        if len(d) != 18 or d[:2] != b'\x12\x01' or d[17] != 1:
            raise ValueError('Expected one configuration and a USB device descriptor')
        self.vid, self.pid, self.revision = struct.unpack_from('<HHH', d, 8)
        if self.vid != 0x045e or self.pid not in (0x0b32,0x0b17,0x0b12):
            raise ValueError('This experimental profile supports only captured Microsoft GIP devices')
        self.product_string = document.get('ProductString', 'Xbox Wireless Headset')
        if not isinstance(self.product_string,str) or not self.product_string or len(self.product_string.encode('utf-16le')) > 252:
            raise ValueError('Invalid USB product string')
        self.virtual_serial = f'XONE{self.pid:04X}TEST'
        if (self.vid, self.pid, self.revision) != (document['Vid'], document['Pid'], document['Revision']):
            raise ValueError('Metadata disagrees with the device descriptor')
        # Windows USB_DEVICE_SPEED and Linux USB/IP usb_device_speed differ.
        self.usbip_speed = {0: 1, 1: 2, 2: 3, 3: 5}[document['WindowsUsbSpeed']]
        self.os_string = bytes.fromhex(document['OsStringDescriptor'])
        if (len(self.os_string) < 18 or self.os_string[0] != len(self.os_string)
                or self.os_string[1] != 3 or self.os_string[2:16] != 'MSFT100'.encode('utf-16le')
                or self.os_string[16] == 0 or self.os_string[17] != 0):
            raise ValueError('Invalid captured Microsoft OS string descriptor')
        self.os_vendor_code = self.os_string[16]
        if len(c) < 9 or c[:2] != b'\x09\x02' or struct.unpack_from('<H', c, 2)[0] != len(c):
            raise ValueError('Invalid complete configuration descriptor')
        self.settings = {}
        self.classes = {}
        current = None
        endpoint_count = 0
        offset = c[0]
        while offset < len(c):
            if len(c)-offset < 2 or c[offset] < 2 or c[offset] > len(c)-offset:
                raise ValueError('Descriptor length exceeds configuration')
            length, kind = c[offset:offset+2]
            part = c[offset:offset+length]
            if kind == 4:
                if length != 9 or (current is not None and len(self.settings[current]) != endpoint_count):
                    raise ValueError('Invalid interface or endpoint count')
                current = (part[2], part[3])
                if current in self.settings:
                    raise ValueError('Duplicate interface alternate setting')
                self.settings[current] = []
                self.classes.setdefault(part[2], tuple(part[5:8]))
                endpoint_count = part[4]
            elif kind == 5:
                if current is None or length != 7:
                    raise ValueError('Endpoint without a valid interface')
                address, attributes = part[2:4]
                size = struct.unpack_from('<H', part, 4)[0]
                if address & 0x70 or not address & 0x0f or size == 0:
                    raise ValueError('Invalid endpoint address or packet size')
                if any(e.address == address for e in self.settings[current]):
                    raise ValueError('Duplicate endpoint within alternate setting')
                self.settings[current].append(Endpoint(address, attributes & 3, size, part[6]))
            else:
                raise ValueError('Unexpected descriptor in this GIP profile')
            offset += length
        if current is None or len(self.settings[current]) != endpoint_count or len(self.classes) != c[4]:
            raise ValueError('Incomplete configuration interfaces/endpoints')
        if any((number, 0) not in self.settings for number in self.classes):
            raise ValueError('Missing default alternate setting')

    @classmethod
    def load(cls, path):
        return cls(json.loads(Path(path).read_text(encoding='utf-8-sig')))

    def usbip_device(self, busid='1-1'):
        bus = busid.encode('ascii')
        if len(bus) > 31 or not bus or b'\0' in bus:
            raise ValueError('Invalid USB/IP bus ID')
        path = b'/xone/virtual-headset/' + bus
        return struct.pack('!256s32sIIIHHH6B', path, bus, 1, 1, self.usbip_speed,
                           self.vid, self.pid, self.revision, *self.device[4:7],
                           self.configuration[5], self.device[17], self.configuration[4])

    def usbip_device_list(self):
        interfaces = b''.join(bytes((*self.classes[n], 0)) for n in sorted(self.classes))
        return struct.pack('!HHII', 0x0111, 0x0005, 0, 1) + self.usbip_device() + interfaces


class ControlState:
    def __init__(self, profile):
        self.profile = profile
        self.configured = 0
        self.alternate = {n: 0 for n in profile.classes}

    @staticmethod
    def string(text):
        encoded = text.encode('utf-16le')
        if len(encoded) > 252:
            raise ValueError('USB string too long')
        return bytes((len(encoded)+2, 3)) + encoded

    def request(self, setup, payload=b''):
        if len(setup) != 8:
            raise ValueError('USB setup packet must contain eight bytes')
        kind, request, value, index, length = struct.unpack('<BBHHH', setup)
        incoming = bool(kind & 0x80)
        if (incoming and payload) or (not incoming and len(payload) != length):
            raise ValueError('Control data stage disagrees with setup')
        p = self.profile
        result = b''
        if kind == 0x80 and request == 6:
            descriptor, number = value >> 8, value & 255
            if descriptor in (1, 2) and number == 0 and index == 0:
                result = p.device if descriptor == 1 else p.configuration
            elif descriptor == 3:
                if number == 0 and index == 0:
                    result = b'\x04\x03\x09\x04'
                elif number == 0xee and index == 0:
                    # Windows caches bVendorCode per VID/PID/revision, so keep
                    # the hardware value rather than inventing another code.
                    result = p.os_string
                elif number in p.device[14:17] and index == 0x0409:
                    strings = {p.device[14]: 'Microsoft', p.device[15]: p.product_string,
                               p.device[16]: p.virtual_serial}
                    result = self.string(strings[number])
                else:
                    raise Stall('Unknown string descriptor')
            else:
                raise Stall('Unsupported descriptor')
        elif kind == 0xc0 and request == p.os_vendor_code and value == 0 and index == 4:
            # Own MS OS compatible-ID descriptor for the observed XGIP10 binding.
            result = struct.pack('<IHHB7xBB8s8s6x', 40, 0x0100, 4, 1, 0, 1, b'XGIP10', b'')
        elif kind == 0 and request == 9 and index == 0 and length == 0 and value in (0, 1):
            self.configured = value
            self.alternate = {n: 0 for n in p.classes}
        elif kind == 0x80 and request == 8 and value == 0 and index == 0:
            result = bytes((self.configured,))
        elif kind == 1 and request == 11 and length == 0 and self.configured and (index, value) in p.settings:
            self.alternate[index] = value
        elif kind == 0x81 and request == 10 and value == 0 and self.configured and index in self.alternate:
            result = bytes((self.alternate[index],))
        elif kind == 0x80 and request == 0 and value == 0 and index == 0:
            result = bytes((bool(p.configuration[7] & 0x40), 0))
        elif kind == 0x81 and request == 0 and value == 0 and self.configured and index in p.classes:
            result = b'\0\0'
        else:
            # Never turn an unimplemented setting/firmware operation into success.
            raise Stall('Unsupported USB control request')
        return result[:length]

    def active_endpoints(self):
        if not self.configured:
            return []
        return [e for n, alt in self.alternate.items() for e in self.profile.settings[(n, alt)]]

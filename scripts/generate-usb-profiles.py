# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile sanitized USB profiles into the C++ bridge's descriptor catalog."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
lines = ['// Generated from driver/profiles by scripts/generate-usb-profiles.py.',
         'Profile profile(uint16_t pid,bool audio){', '    switch(pid){']
for filename in ('headset-0b32-usb.json', 'headset-0b17-usb.json', 'controller-0b12-usb.json'):
    p = json.loads((root / 'driver/profiles' / filename).read_text(encoding='utf-8-sig'))
    device, config, os = (p[k] for k in ('DeviceDescriptor', 'ConfigurationDescriptor', 'OsStringDescriptor'))
    name = p.get('ProductString', 'Xbox Wireless Headset')
    if not name.isascii():
        raise ValueError('Expected an ASCII profile product name')
    speed = {0:1, 1:2, 2:3, 3:5}[p['WindowsUsbSpeed']]
    lines.append(f'    case 0x{p["Pid"]:04x}:return Profile(pid,{p["Revision"]},audio?3:{speed},'
                 f'hex_bytes("{device}"),hex_bytes("{config}"),hex_bytes("{os}"),{json.dumps(name)});')
lines += ['    default:throw Error("Unsupported captured USB profile");', '    }', '}']
(root / 'src/usbip_native_profiles.inc').write_text('\n'.join(lines)+'\n', encoding='utf-8', newline='\n')

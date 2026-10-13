"""Summarize the observed XboxGip WPP layout, without exporting host randomness.

Message GUID/IDs were checked against the local XboxGip public PDB and binary.
This is a version-specific diagnostic decoder, not a general WPP decoder.
"""
import collections
import json
import re
import statistics
import sys
from pathlib import Path

SECURITY = '3a504637-90ca-3d8b-ee65-04d74ba7a21f'
WIRE_DUMP = '969104f8-d416-3757-4268-7e88d504a836'
rows = [json.loads(line) for line in Path(sys.argv[1]).read_text(encoding='utf-8-sig').splitlines()]
messages = []
current = None
errors = []
for row in rows:
    data = bytes.fromhex(row['Data'])
    if row['Guid'] == SECURITY and row['Id'] == 20 and len(data) == 16:
        errors.append((row['Time100ns'], int.from_bytes(data[12:16], 'little')))
    if row['Guid'] != WIRE_DUMP or row['Id'] != 14:
        continue
    text = data.split(b'\0', 1)[0].decode('ascii', errors='replace')
    match = re.fullmatch(r'(Host|Device): ([0-9a-fA-F]{4}) ([0-9a-fA-F ]+)', text)
    if not match:
        continue
    direction, offset, chunk = match.groups()
    offset = int(offset, 16)
    if offset == 0:
        current = {'Direction': direction, 'Time100ns': row['Time100ns'], 'Data': bytearray()}
        messages.append(current)
    if current is None or current['Direction'] != direction or len(current['Data']) != offset:
        raise ValueError('Inconsistent WPP hex dump offsets')
    current['Data'].extend(bytes.fromhex(chunk))

headers = collections.Counter((message['Direction'], len(message['Data']), message['Data'][:9].hex()) for message in messages)
# XboxGip strips the outer security-client byte before this particular dump.
empty_v3 = [m for m in messages if m['Direction'] == 'Device' and m['Data'] == bytes.fromhex('c20002000402030000')]
errors_by_status = collections.Counter(f'0x{value:08x}' for _, value in errors)
cycles = [r['Time100ns'] for r in rows if r['Guid'] == SECURITY and r['Id'] == 38]
summary = {
    'Events': len(rows),
    'HostAuthHelloV1Count': sum(m['Direction'] == 'Host' and m['Data'][:9] == bytes.fromhex('410001002c01010028') for m in messages),
    'EmptyVersion3HelloCount': len(empty_v3),
    'DevAuthProcessDeviceMessageErrors': errors_by_status,
    'AuthKickoffCount': len(cycles),
    'MedianKickoffIntervalSeconds': statistics.median((b-a)/1e7 for a, b in zip(cycles, cycles[1:])) if len(cycles)>1 else None,
    'Headers': [{'Direction': direction, 'Bytes': size, 'First9Bytes': header, 'Count': count} for (direction, size, header), count in headers.items()],
    'AuthVersion3Implemented': False,
    'Scope': 'Observed stock XboxGip security/auth trace; does not determine a working filter design.',
}
print(json.dumps(summary, ensure_ascii=False, indent=2))

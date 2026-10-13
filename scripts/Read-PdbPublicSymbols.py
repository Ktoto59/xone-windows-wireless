"""Read public symbol names and section offsets from a local MSF 7 PDB."""
import argparse
from pathlib import Path
import struct


def symbols(path):
    data = Path(path).read_bytes()
    if not data.startswith(b'Microsoft C/C++ MSF 7.00'):
        raise ValueError('Expected MSF 7 PDB')
    block_size, _, _, directory_size, _, block_map = struct.unpack_from('<6I', data, 32)
    if block_size not in (512, 1024, 2048, 4096):
        raise ValueError('Unsupported PDB block size')
    def blocks_at(offset, size):
        count = (size+block_size-1)//block_size
        block_ids = struct.unpack_from('<'+'I'*count, data, offset)
        for block_id in block_ids:
            if (block_id+1)*block_size > len(data):
                raise ValueError('PDB block outside file')
        return b''.join(data[b*block_size:(b+1)*block_size] for b in block_ids)[:size]
    directory = blocks_at(block_map*block_size, directory_size)
    count = struct.unpack_from('<I', directory)[0]
    sizes = struct.unpack_from('<'+'I'*count, directory, 4)
    streams = []
    offset = 4+4*count
    for size in sizes:
        if size == 0xffffffff:
            streams.append(b'')
            continue
        block_count = (size+block_size-1)//block_size
        ids = struct.unpack_from('<'+'I'*block_count, directory, offset)
        offset += 4*block_count
        if any((b+1)*block_size > len(data) for b in ids):
            raise ValueError('PDB stream outside file')
        streams.append(b''.join(data[b*block_size:(b+1)*block_size] for b in ids)[:size])
    index = struct.unpack_from('<H', streams[3], 20)[0]
    records = streams[index]
    offset = 0
    while offset+4 <= len(records):
        size, kind = struct.unpack_from('<HH', records, offset)
        if size < 2 or offset+size+2 > len(records):
            raise ValueError('Invalid PDB symbol record')
        if kind == 0x110e and size >= 12:
            flags, address, section = struct.unpack_from('<IIH', records, offset+4)
            name = records[offset+14:offset+2+size].split(b'\0')[0].decode('utf-8', errors='replace')
            yield section, address, name
        offset += size+2


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('filters', nargs='*')
    args = parser.parse_args()
    for section, address, name in sorted(symbols(args.path)):
        if not args.filters or any(word.lower() in name.lower() for word in args.filters):
            print(f'{section}:{address:08x} {name}')

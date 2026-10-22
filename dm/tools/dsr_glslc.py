#!/usr/bin/env python3
# Extract GLSLC compiler outputs from Dark Souls Remastered's BHD5/BDT archives (DCX zlib / BND3 entries).
# Output: <out>/<sha16>.glslc, the glslc_capture format shader_split.py reads with shader_glslc_capture on.
# Usage: dsr_glslc.py <RomFS> <out dir>
import argparse, hashlib, mmap, struct, sys, zlib
from pathlib import Path

import bhd5

MAGIC = b'\x91\x68\x86\x19'


def extract(romfs, out):
    archives = sorted(romfs.glob('*.bhd5'))
    if not archives: raise ValueError(f'no BHD5 archives in {romfs}')
    out.mkdir(parents=True, exist_ok=True)
    seen = set()
    for header in archives:
        with header.with_suffix('.bdt').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for _, size, offset in bhd5.entries(header):
                if offset + size > len(data): raise ValueError(f'{header.name}: entry outside the BDT file at {offset:#x}')
                entry = data[offset:offset + size]
                if entry[:4] == b'DCX\0':
                    entry = zlib.decompress(entry[0x4c:])   # Switch DFLT DCX header, then the zlib stream
                i = entry.find(MAGIC)
                while i >= 0:
                    length = struct.unpack_from('<I', entry, i + 0x44)[0] if i + 0x48 <= len(entry) else 0
                    if 0x760 < length <= len(entry) - i:
                        blob = entry[i:i + length]; key = hashlib.sha256(blob).hexdigest()[:16]
                        if key not in seen:
                            seen.add(key); (out / f'{key}.glslc').write_bytes(blob)
                    i = entry.find(MAGIC, i + 1)
    if not seen: raise ValueError(f'no GLSLC outputs in {romfs}')
    return len(seen)


def main():
    ap = argparse.ArgumentParser(description='Extract GLSLC outputs from BHD5/BDT archives into a glslc_capture folder.')
    ap.add_argument('romfs', type=Path)
    ap.add_argument('out', type=Path)
    args = ap.parse_args()
    try: count = extract(args.romfs, args.out)
    except (OSError, ValueError, zlib.error, struct.error) as e: sys.exit(f'[RED] GLSLC extraction failed: {e}')
    print(count, 'unique GLSLC outputs written to', args.out)


if __name__ == '__main__': main()

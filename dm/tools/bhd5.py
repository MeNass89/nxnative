#!/usr/bin/env python3
# Dark Souls Remastered's Switch BHD5/BDT archives: bucket count and table offset (u64) at +0x10;
# buckets {count u32, pad u32, offset u64}, entries {hash u32, size u32, offset u64}.
# Usage: bhd5.py <archive.bhd5> [scan <bytes-hex>]
import argparse, collections, mmap, struct, sys
from pathlib import Path


def entries(bhd):
    h = Path(bhd).read_bytes()
    if len(h) < 0x20 or h[:4] != b'BHD5': raise ValueError(f'{bhd}: not a Switch BHD5 header')
    count, table = struct.unpack_from('<QQ', h, 0x10)
    if table + count * 16 > len(h): raise ValueError(f'{bhd}: bucket table outside the header')
    out = []
    for b in range(count):
        n, _, offset = struct.unpack_from('<IIQ', h, table + b * 16)
        if offset + n * 16 > len(h): raise ValueError(f'{bhd}: entry table outside the header')
        for e in range(n): out.append(struct.unpack_from('<IIQ', h, offset + e * 16))
    return out


def main():
    ap = argparse.ArgumentParser(description='List or scan a Switch BHD5/BDT archive.')
    ap.add_argument('bhd', type=Path)
    ap.add_argument('command', nargs='?', choices=['scan'])
    ap.add_argument('pattern', nargs='?', help='hex bytes to scan for')
    args = ap.parse_args()
    if args.command and not args.pattern: ap.error('scan requires a hex byte pattern')
    try:
        es = entries(args.bhd)
        with args.bhd.with_suffix('.bdt').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            mags = collections.Counter(data[o:o + 4] for _, _, o in es)
            print(len(es), 'entries', mags.most_common(15))
            if args.command:
                pattern = bytes.fromhex(args.pattern)
                if not pattern: raise ValueError('scan pattern must not be empty')
                i = data.find(pattern); hits = []
                while i >= 0 and len(hits) < 20:
                    e = next(((hs, sz, o) for hs, sz, o in es if o <= i < o + sz), None)
                    hits.append((hex(i), e and (hex(e[0]), e[1], hex(e[2]), data[e[2]:e[2] + 4])))
                    i = data.find(pattern, i + 1)
                print(hits)
    except (OSError, ValueError, struct.error) as e: sys.exit(f'[RED] BHD5 read failed: {e}')


if __name__ == '__main__': main()

#!/usr/bin/env python3
"""Inspect the runner's 128 KiB RVNDSV01 melonDS save container.

Understands both the old 16-entry metadata header and the canonical 4-entry
(three character slots + option) layout. It also validates Ragnarok's own
encoded saveN length/checksum without modifying the file.
"""
from __future__ import annotations
import argparse, struct, zlib
from pathlib import Path

MAGIC=b'RVNDSV01'
META_SIZE=0x1000
ENTRY_BASE=64
ENTRY_SIZE=64
META_CRC_OFF=META_SIZE-4
VALID_COUNTS={4,16}


def u32(b: bytes, off: int) -> int:
    return struct.unpack_from('<I',b,off)[0]


def guest_slot_ok(name: str, data: bytes) -> bool|None:
    if name not in {'save0','save1','save2','option','list'}:
        return None
    if len(data)<4:
        return False
    if data[0] | (data[2]<<8) != len(data):
        return False
    check=0
    for i in range(4,len(data)):
        check ^= data[i] ^ data[i%3] ^ (i & 0xff)
    return check == data[3]


def inspect(path: Path) -> int:
    b=path.read_bytes()
    if len(b)!=128*1024:
        print(f'ERROR: size={len(b)}; expected 131072')
        return 2
    if b[:8]!=MAGIC:
        print(f'ERROR: magic={b[:8]!r}; expected {MAGIC!r}')
        return 2
    version=u32(b,8); generation=u32(b,12); count=u32(b,16); meta_size=u32(b,20)
    stored_meta_crc=u32(b,META_CRC_OFF)
    actual_meta_crc=zlib.crc32(b[:META_CRC_OFF]) & 0xffffffff
    print(f'{path}: version={version} generation={generation} entries={count} meta={meta_size}')
    print(f'metadata CRC: {"OK" if stored_meta_crc==actual_meta_crc else "BAD"}')
    if version!=1 or count not in VALID_COUNTS or meta_size!=META_SIZE:
        print('ERROR: unsupported metadata layout')
        return 2
    found=[]
    for i in range(count):
        e=b[ENTRY_BASE+i*ENTRY_SIZE:ENTRY_BASE+(i+1)*ENTRY_SIZE]
        if e[0] in (0,0xff):
            continue
        name=e[:48].split(b'\0',1)[0].decode('ascii','replace')
        off,length,crc,flags=struct.unpack_from('<IIII',e,48)
        data=b[off:off+length] if off+length<=len(b) else b''
        crc_ok=len(data)==length and (zlib.crc32(data)&0xffffffff)==crc
        gok=guest_slot_ok(name,data)
        guest='' if gok is None else f' guest-check={"OK" if gok else "BAD"}'
        print(f'[{i}] {name}: off=0x{off:x} len={length} flags={flags} crc={"OK" if crc_ok else "BAD"}{guest}')
        found.append(name)
    bad=[n for n in found if n.startswith('save') and n not in {'save0','save1','save2'}]
    if bad:
        print('legacy/non-game save names:', ', '.join(bad))
    missing=[n for n in ('option','list') if n not in found]
    if missing:
        print('WARNING: missing persistent metadata:', ', '.join(missing))
    if 'list' not in found and any(n in found for n in ('save0','save1','save2')):
        print('WARNING: orphaned character save: title menu will show New Game because list is absent')
    return 0


def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument('sav',type=Path)
    args=ap.parse_args()
    return inspect(args.sav)

if __name__=='__main__': raise SystemExit(main())

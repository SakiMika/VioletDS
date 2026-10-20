#!/usr/bin/env python3
"""Prepare Ragnarok Violet JAR assets for the NDS/DSi port.

Important: Ragnarok does not treat every PNG as an opaque image blob.  Its
native code parses embedded PNG chunk structure directly (notably PLTE) and
mutates palette bytes before calling MC_grpCreateImage.  Therefore the port
MUST preserve the original PNG bytes in NitroFS.

To avoid shipping a full zlib/PNG decoder on ARM9, this tool also builds a
sidecar cache containing the already-decompressed palette-index plane for every
unique PNG IDAT stream.  At runtime CreateImage parses the *current* PNG palette
(including game mutations), identifies the unchanged IDAT stream by hash, reads
the cached index plane, and expands it to RGB565 + alpha.

Outputs:
  nitrofs/...                       pristine JAR payload files
  nitrofs/_port/rv_png_indices.bin decompressed palette-index planes
  source/rv_png_cache_table.h      lookup metadata keyed by IDAT hash

Requires Pillow only when re-packing the original JAR.
"""
from __future__ import annotations

import argparse
import io
import shutil
import struct
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from PIL import Image

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
FNV_OFFSET = 2166136261
FNV_PRIME = 16777619


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFF
    return h


def png_exact_length(blob: bytes, start: int) -> int:
    if blob[start : start + 8] != PNG_MAGIC:
        raise ValueError("not a PNG")
    pos = start + 8
    while True:
        if pos + 12 > len(blob):
            raise ValueError("truncated PNG chunk header")
        size = struct.unpack_from(">I", blob, pos)[0]
        typ = blob[pos + 4 : pos + 8]
        end = pos + 12 + size
        if end > len(blob):
            raise ValueError("truncated PNG chunk")
        pos = end
        if typ == b"IEND":
            return pos - start


def png_meta(png: bytes):
    if not png.startswith(PNG_MAGIC):
        raise ValueError("PNG signature missing")
    pos = 8
    ihdr = None
    idat = bytearray()
    while True:
        if pos + 12 > len(png):
            raise ValueError("truncated PNG")
        size = struct.unpack_from(">I", png, pos)[0]
        typ = png[pos + 4 : pos + 8]
        data = png[pos + 8 : pos + 8 + size]
        if pos + 12 + size > len(png):
            raise ValueError("truncated PNG payload")
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", data)
        elif typ == b"IDAT":
            idat += data
        pos += 12 + size
        if typ == b"IEND":
            break
    if ihdr is None:
        raise ValueError("PNG lacks IHDR")
    w, h, bit_depth, color_type, compression, filt, interlace = ihdr
    if color_type != 3 or compression != 0 or filt != 0 or interlace not in (0, 1):
        raise ValueError(
            f"unsupported PNG mode {w}x{h} bd={bit_depth} ct={color_type} "
            f"c={compression} f={filt} i={interlace}"
        )
    return w, h, bit_depth, interlace, bytes(idat)


@dataclass(frozen=True)
class Key:
    crc32: int
    fnv1a: int
    idat_len: int
    width: int
    height: int
    bit_depth: int
    interlace: int


@dataclass
class CacheRecord:
    key: Key
    offset: int
    pixels: int


def extract_pngs(blob: bytes):
    pos = 0
    while True:
        start = blob.find(PNG_MAGIC, pos)
        if start < 0:
            return
        size = png_exact_length(blob, start)
        yield start, blob[start : start + size]
        pos = start + size


def decode_indices(png: bytes, width: int, height: int) -> bytes:
    with Image.open(io.BytesIO(png)) as im:
        im.load()
        if im.mode != "P":
            # Indexed-color PNGs should remain P even when bit depth is 1.
            # Refuse accidental palette remapping rather than silently changing
            # the game's pixel indices.
            raise ValueError(f"expected Pillow P mode, got {im.mode}")
        raw = im.tobytes()
    if len(raw) != width * height:
        raise ValueError(f"decoded index plane {len(raw)} != {width}x{height}")
    return raw


def write_header(path: Path, records: list[CacheRecord]) -> None:
    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "",
        "struct RvPngCacheEntry {",
        "    uint32_t crc32;",
        "    uint32_t fnv1a;",
        "    uint32_t idat_len;",
        "    uint32_t offset;",
        "    uint32_t pixels;",
        "    uint16_t width;",
        "    uint16_t height;",
        "    uint8_t bit_depth;",
        "    uint8_t interlace;",
        "    uint16_t reserved;",
        "};",
        "",
        f"static const uint32_t kRvPngCacheEntryCount = {len(records)}u;",
        "static const RvPngCacheEntry kRvPngCacheEntries[] = {",
    ]
    for r in records:
        k = r.key
        lines.append(
            "    {0x%08xu,0x%08xu,%uu,%uu,%uu,%uu,%uu,%uu,%uu,0u},"
            % (
                k.crc32,
                k.fnv1a,
                k.idat_len,
                r.offset,
                r.pixels,
                k.width,
                k.height,
                k.bit_depth,
                k.interlace,
            )
        )
    lines += ["};", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("jar", type=Path, help="Ragnarok Violet JAR")
    ap.add_argument("out", type=Path, nargs="?", default=Path("nitrofs"))
    ap.add_argument(
        "--header",
        type=Path,
        default=Path("source/rv_png_cache_table.h"),
        help="generated C++ lookup header",
    )
    args = ap.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    args.header.parent.mkdir(parents=True, exist_ok=True)

    # Preserve game resources byte-for-byte.
    files = 0
    png_count = 0
    cache_by_key: dict[Key, tuple[bytes, str]] = {}
    with zipfile.ZipFile(args.jar) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            if name.upper().startswith("META-INF/"):
                continue
            data = zf.read(info)
            dst = args.out / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
            files += 1

            for start, png in extract_pngs(data):
                w, h, bit_depth, interlace, idat = png_meta(png)
                key = Key(
                    zlib.crc32(idat) & 0xFFFFFFFF,
                    fnv1a(idat),
                    len(idat),
                    w,
                    h,
                    bit_depth,
                    interlace,
                )
                indices = decode_indices(png, w, h)
                desc = f"{name}@{start}"
                old = cache_by_key.get(key)
                if old is not None and old[0] != indices:
                    raise ValueError(f"hash collision with different indices: {old[1]} vs {desc}")
                cache_by_key.setdefault(key, (indices, desc))
                png_count += 1

    port_dir = args.out / "_port"
    port_dir.mkdir(parents=True, exist_ok=True)
    cache_path = port_dir / "rv_png_indices.bin"
    records: list[CacheRecord] = []
    offset = 0
    with cache_path.open("wb") as f:
        for key in sorted(
            cache_by_key,
            key=lambda k: (k.crc32, k.fnv1a, k.width, k.height, k.bit_depth, k.interlace, k.idat_len),
        ):
            indices = cache_by_key[key][0]
            f.write(indices)
            records.append(CacheRecord(key, offset, len(indices)))
            offset += len(indices)

    write_header(args.header, records)

    game_bytes = sum(
        p.stat().st_size
        for p in args.out.rglob("*")
        if p.is_file() and "_port" not in p.relative_to(args.out).parts
    )
    print(
        f"packed {files} pristine game files ({game_bytes} bytes), "
        f"found {png_count} PNG instances / {len(records)} unique IDAT streams; "
        f"cache={offset} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

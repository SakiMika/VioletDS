#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Zenonia WIPI/DSi AIO v1.3 — một file, thư viện chuẩn Python 3.
Chạy trực tiếp để mở menu, hoặc:
  python zenonia_wipi_aio_tool.py export DU_AN DUMP --no-graphics
  python zenonia_wipi_aio_tool.py import DU_AN DUMP DU_AN_MOI

Hãy export lại vào thư mục dump MỚI để thấy chữ còn thiếu:
  txt/binary.mod_raw.txt: gồm 482 ô native đã xác minh, cả 저/중/고.
  txt/menu_Title.gui_raw.txt: nhãn OK : 선택 và các ô nhập ID/tên nhóm.
Tên dump đã làm sạch có thể khác theo quy tắc safe_name của tool.
Chỉ dòng FIXED_SLOT được sửa trong file RAW, không được quá max BYTE.
Ví dụ Detail dùng L/M/H; OK : 선택 có thể dùng OK: Pick.
Dòng RAW khác/GXL chưa xác minh vẫn chỉ tham khảo và bị chặn khi sửa.
DLG là RAW offset32, KHÔNG nén lại; ZT1 mới dùng zlib.
Không tự dịch, không thay font, không build ROM. Sau import cần clean/build.
Giữ bản nguồn/dump cũ để dự phòng. Đầu ra luôn phải là đường dẫn mới.
Layout fixed-slot được xác minh bằng hash mọi byte ngoài vùng chữ; nếu
binary không khớp v041, chỉ dump RAW tham khảo, không ghi chuỗi native mù.
Title.gui khác layout đã biết sẽ bị từ chối thay vì đoán offset.
"""
from __future__ import annotations

import csv, json, os, re, shutil, struct, sys, traceback, zipfile, zlib
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

import argparse, hashlib, tempfile, unicodedata

DUMP_DIR_NAME = "zenonia_wipi_text_dump"
HAS_PZX = True
TEXT_ENCODING = "cp949"
ENCODE_ERRORS = "strict"
WRITE_ENCODING = None
CONTROL_POLICY = "compatible"
VI_ORDER = 'áàảãạâấầẩẫậăắằẳẵặđéèẻẽẹêếềểễệíìỉĩịóòỏõọôốồổỗộơớờởỡợúùủũụưứừửữựýỳỷỹỵ'
VI_MAP = {ch: 0x80 + i for i, ch in enumerate(VI_ORDER)}
VI_REV = {0x80 + i: ch for i, ch in enumerate(VI_ORDER)}
PRINTABLE = set(range(0x20, 0x7F))
UNKNOWN_REPLACEMENT_BYTE = 0x3F  # '?' - dùng khi gặp ký tự ngoài bảng 1-byte
UNKNOWN_REPLACEMENT_CHAR = '?'


class PZXError(Exception):
    pass

class MiniImage:
    def __init__(self, width: int, height: int, fill=(0, 0, 0, 0)):
        self.width = int(width)
        self.height = int(height)
        self.size = (self.width, self.height)
        r, g, b, a = fill
        self.pixels = bytearray([r, g, b, a]) * (self.width * self.height)

    def set_pixel(self, x: int, y: int, rgba) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            i = (y * self.width + x) * 4
            self.pixels[i:i+4] = bytes(rgba)

    def paste(self, src: 'MiniImage', x0: int, y0: int) -> None:
        for y in range(src.height):
            dy = y0 + y
            if dy < 0 or dy >= self.height:
                continue
            s0 = y * src.width * 4
            d0 = (dy * self.width + x0) * 4
            if x0 >= 0 and x0 + src.width <= self.width:
                self.pixels[d0:d0 + src.width * 4] = src.pixels[s0:s0 + src.width * 4]
            else:
                for x in range(src.width):
                    dx = x0 + x
                    if 0 <= dx < self.width:
                        si = (y * src.width + x) * 4
                        di = (dy * self.width + dx) * 4
                        self.pixels[di:di+4] = src.pixels[si:si+4]

    def alpha_composite(self, src: 'MiniImage', dest) -> None:
        self.paste(src, int(dest[0]), int(dest[1]))

    def save(self, filename) -> None:
        filename = Path(filename)
        filename.parent.mkdir(parents=True, exist_ok=True)
        def chunk(tag: bytes, data: bytes) -> bytes:
            import binascii
            return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', binascii.crc32(tag + data) & 0xFFFFFFFF)
        raw = bytearray()
        row_len = self.width * 4
        for y in range(self.height):
            raw.append(0)  # PNG filter none
            raw.extend(self.pixels[y * row_len:(y + 1) * row_len])
        png = [b'\x89PNG\r\n\x1a\n']
        png.append(chunk(b'IHDR', struct.pack('>IIBBBBB', self.width, self.height, 8, 6, 0, 0, 0)))
        png.append(chunk(b'IDAT', zlib.compress(bytes(raw), 9)))
        png.append(chunk(b'IEND', b''))
        filename.write_bytes(b''.join(png))

def u16le(b: bytes, off: int) -> int:
    return struct.unpack_from('<H', b, off)[0]

def u32le(b: bytes, off: int) -> int:
    return struct.unpack_from('<I', b, off)[0]

def rgb565_to_rgba(v: int, alpha: int = 255):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return (r, g, b, alpha)

def find_pzd_section(blob: bytes):
    if blob[:4] == b'PZX\x01':
        if len(blob) < 16:
            raise PZXError('PZX header is truncated')
        pzd_off = u32le(blob, 4)
        pzf_off = u32le(blob, 8)
        pza_off = u32le(blob, 12)
        if pzd_off >= len(blob):
            raise PZXError(f'Bad PZD offset: 0x{pzd_off:X}')
        section_end_candidates = [x for x in (pzf_off, pza_off, len(blob)) if x > pzd_off]
        section_end = min(section_end_candidates) if section_end_candidates else len(blob)
        return pzd_off, section_end, {'container': 'PZX', 'pzd_off': pzd_off, 'pzf_off': pzf_off, 'pza_off': pza_off, 'section_end': section_end}
    if blob[:4] == b'PZD\x02':
        return 4, len(blob), {'container': 'PZD', 'pzd_off': 4, 'section_end': len(blob)}
    raise PZXError('Not a supported PZX/PZD file')

def decode_zero_image(encoded: bytes, width: int, height: int, palette, index: int = 0, mode: int = 2):
    img = MiniImage(width, height, (0, 0, 0, 0))
    p = 0
    x = 0
    y = 0
    n = len(encoded)
    while p + 2 <= n:
        cmd = u16le(encoded, p)
        p += 2
        if cmd == 0xFFFF:
            break
        if cmd == 0xFFFE:
            y += 1
            x = 0
            if y > height:
                break
            continue
        if cmd & 0x8000:
            count = cmd & 0x7FFF
            if mode == 1:
                need = (count + 1) // 2
                if p + need > n:
                    raise PZXError(f'Image {index}: 4bpp copy run exceeds encoded stream')
                for k in range(count):
                    byte = encoded[p + (k // 2)]
                    pal_index = (byte >> 4) if (k % 2 == 0) else (byte & 0x0F)
                    img.set_pixel(x, y, palette[pal_index] if pal_index < len(palette) else (255, 0, 255, 255))
                    x += 1
                p += need
            else:
                if p + count > n:
                    raise PZXError(f'Image {index}: copy run exceeds encoded stream')
                for k in range(count):
                    pal_index = encoded[p + k]
                    img.set_pixel(x, y, palette[pal_index] if pal_index < len(palette) else (255, 0, 255, 255))
                    x += 1
                p += count
        else:
            x += cmd
    return img

def decode_image_with_local_palette(buf: bytes, off: int, index: int = 0):
    if off < 0 or off + 1 >= len(buf):
        raise PZXError(f'Image {index}: bad offset 0x{off:X}')
    pos = off
    palette_count = buf[pos]
    pos += 1
    if palette_count <= 0:
        raise PZXError(f'Image {index}: empty local palette')
    if pos + palette_count * 2 + 16 > len(buf):
        raise PZXError(f'Image {index}: local palette/header truncated')
    palette565 = [u16le(buf, pos + i * 2) for i in range(palette_count)]
    palette = [rgb565_to_rgba(v) for v in palette565]
    pos += palette_count * 2
    width = u16le(buf, pos)
    height = u16le(buf, pos + 2)
    mode = buf[pos + 4]
    pad = buf[pos + 5:pos + 8]
    data_size = u32le(buf, pos + 8)
    img_comp_size = u32le(buf, pos + 12)
    data_pos = pos + 16
    if not (1 <= width <= 4096 and 1 <= height <= 4096):
        raise PZXError(f'Image {index}: unlikely size {width}x{height}')
    if img_comp_size:
        if data_pos + img_comp_size > len(buf):
            raise PZXError(f'Image {index}: compressed data truncated')
        encoded = zlib.decompress(buf[data_pos:data_pos + img_comp_size])
        if len(encoded) != data_size:
            raise PZXError(f'Image {index}: bad decompressed size {len(encoded)} != {data_size}')
    else:
        if data_pos + data_size > len(buf):
            raise PZXError(f'Image {index}: raw data truncated')
        encoded = buf[data_pos:data_pos + data_size]
    img = decode_zero_image(encoded, width, height, palette, index=index, mode=mode)
    meta = {'index': index, 'offset': off, 'width': width, 'height': height, 'mode': mode, 'pad_hex': pad.hex(), 'palette_count': palette_count, 'data_size': data_size, 'compressed_size': img_comp_size}
    return img, meta

def parse_type8_shared_palette(blob: bytes, start: int, image_type: int, image_count: int, unknown: int, flags: int, meta: dict):
    pos = start + 4
    if pos >= len(blob):
        raise PZXError('Missing palette count')
    palette_count = blob[pos]
    pos += 1
    if pos + palette_count * 2 > len(blob):
        raise PZXError('Palette is truncated')
    palette565 = [u16le(blob, pos + i * 2) for i in range(palette_count)]
    palette = [rgb565_to_rgba(v) for v in palette565]
    pos += palette_count * 2
    if pos + 8 > len(blob):
        raise PZXError('Missing ZeroPZD zlib size fields')
    raw_size = u32le(blob, pos)
    comp_size = u32le(blob, pos + 4)
    pos += 8
    if pos + comp_size > len(blob):
        raise PZXError('Compressed ZeroPZD payload is truncated')
    payload = zlib.decompress(blob[pos:pos + comp_size])
    if raw_size != len(payload):
        raise PZXError(f'Bad payload size: header says {raw_size}, zlib gave {len(payload)}')
    if image_count * 4 > len(payload):
        raise PZXError('Image index table is truncated')
    offsets = [u32le(payload, i * 4) for i in range(image_count)]
    images = []
    decoded_meta = []
    for index, off in enumerate(offsets):
        if off + 16 > len(payload):
            raise PZXError(f'Image {index}: bad offset 0x{off:X}')
        width = u16le(payload, off)
        height = u16le(payload, off + 2)
        mode = payload[off + 4]
        data_size = u32le(payload, off + 8)
        img_comp_size = u32le(payload, off + 12)
        data_pos = off + 16
        if img_comp_size:
            if data_pos + img_comp_size > len(payload):
                raise PZXError(f'Image {index}: compressed data truncated')
            encoded = zlib.decompress(payload[data_pos:data_pos + img_comp_size])
            if len(encoded) != data_size:
                raise PZXError(f'Image {index}: bad decompressed size {len(encoded)} != {data_size}')
        else:
            if data_pos + data_size > len(payload):
                raise PZXError(f'Image {index}: raw data truncated')
            encoded = payload[data_pos:data_pos + data_size]
        img = decode_zero_image(encoded, width, height, palette, index=index, mode=mode)
        images.append(img)
        decoded_meta.append({'index': index, 'offset': off, 'width': width, 'height': height, 'mode': mode, 'palette_count': palette_count, 'data_size': data_size, 'compressed_size': img_comp_size})
    meta.update({'variant': 'type8_shared_palette', 'palette_count': palette_count, 'payload_size': raw_size, 'decoded_images': decoded_meta})
    return images, meta

def parse_type8_local_palette(blob: bytes, start: int, image_type: int, image_count: int, unknown: int, flags: int, meta: dict):
    pos = start + 4
    if pos + 8 > len(blob):
        raise PZXError('Missing local-palette payload size fields')
    raw_size = u32le(blob, pos)
    comp_size = u32le(blob, pos + 4)
    pos += 8
    if pos + comp_size > len(blob):
        raise PZXError('Compressed local-palette payload is truncated')
    payload = zlib.decompress(blob[pos:pos + comp_size])
    if len(payload) != raw_size:
        raise PZXError(f'Bad local-palette payload size {len(payload)} != {raw_size}')
    if image_count * 4 > len(payload):
        raise PZXError('Local-palette image table truncated')
    offsets = [u32le(payload, i * 4) for i in range(image_count)]
    images = []
    decoded_meta = []
    for index, off in enumerate(offsets):
        img, imeta = decode_image_with_local_palette(payload, off, index=index)
        images.append(img)
        decoded_meta.append(imeta)
    meta.update({'variant': 'type8_local_palette_zlib', 'payload_size': raw_size, 'decoded_images': decoded_meta})
    return images, meta

def parse_type7_local_palette(blob: bytes, start: int, section_end: int, image_type: int, image_count: int, unknown: int, flags: int, meta: dict):
    table_pos = start + 4
    if table_pos + image_count * 4 > len(blob):
        raise PZXError('Type7 image table truncated')
    raw_offsets = [u32le(blob, table_pos + i * 4) for i in range(image_count)]
    table_end = table_pos + image_count * 4
    if raw_offsets and all(table_end <= off < section_end for off in raw_offsets):
        offsets = raw_offsets
        offset_base = 'absolute_file'
    elif raw_offsets and all(start + off < section_end for off in raw_offsets):
        offsets = [start + off for off in raw_offsets]
        offset_base = 'section_relative'
    else:
        raise PZXError('Type7 image offsets are outside section')
    images = []
    decoded_meta = []
    for index, off in enumerate(offsets):
        img, imeta = decode_image_with_local_palette(blob, off, index=index)
        images.append(img)
        imeta['raw_offset_value'] = raw_offsets[index]
        decoded_meta.append(imeta)
    meta.update({'variant': 'type7_local_palette', 'offset_base': offset_base, 'decoded_images': decoded_meta})
    return images, meta

def parse_pzx(blob: bytes):
    start, section_end, meta = find_pzd_section(blob)
    if start + 4 > len(blob):
        raise PZXError('PZD section is truncated')
    image_type, image_count, unknown, flags = blob[start:start + 4]
    meta.update({'image_type': image_type, 'image_count': image_count, 'unknown': unknown, 'flags': flags})
    if image_type == 8 and flags != 0:
        return parse_type8_shared_palette(blob, start, image_type, image_count, unknown, flags, meta)
    if image_type == 8 and flags == 0:
        return parse_type8_local_palette(blob, start, image_type, image_count, unknown, flags, meta)
    if image_type == 7 and flags == 0:
        return parse_type7_local_palette(blob, start, section_end or len(blob), image_type, image_count, unknown, flags, meta)
    raise PZXError(f'Unsupported PZD variant: type={image_type}, flags={flags}')

def make_atlas_labeled(images, pad: int = 8, max_width: int = 1024, label_h: int = 0):
    # Fallback stdlib: atlas không vẽ số label, tên file PNG riêng đã có index 000/001/...
    if not images:
        return MiniImage(1, 1, (0, 0, 0, 0))
    rows = []
    row = []
    row_w = 0
    row_h = 0
    for im in images:
        w, h = im.width, im.height
        add_w = w + (pad if row else 0)
        if row and row_w + add_w > max_width:
            rows.append((row, row_w, row_h))
            row = []
            row_w = 0
            row_h = 0
        if row:
            row_w += pad
        row.append(im)
        row_w += w
        row_h = max(row_h, h)
    if row:
        rows.append((row, row_w, row_h))
    atlas_w = max(rw for _, rw, _ in rows)
    atlas_h = sum(rh for _, _, rh in rows) + pad * (len(rows) - 1)
    atlas = MiniImage(atlas_w, atlas_h, (0, 0, 0, 0))
    y = 0
    for row, _, rh in rows:
        x = 0
        for im in row:
            atlas.paste(im, x, y)
            x += im.width + pad
        y += rh + pad
    return atlas


def safe_name(path: str) -> str:
    return ''.join(ch if ch.isalnum() or ch in '._-' else '_' for ch in path)


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def copy_zipinfo(src: zipfile.ZipInfo) -> zipfile.ZipInfo:
    zi = zipfile.ZipInfo(src.filename, date_time=src.date_time)
    zi.compress_type = src.compress_type
    zi.comment = src.comment
    zi.extra = src.extra
    zi.internal_attr = src.internal_attr
    zi.external_attr = src.external_attr
    zi.create_system = src.create_system
    return zi


def bytes_to_edit_text(data: bytes, strip_nul: bool = False) -> Tuple[str, int]:
    nul = len(data) - len(data.rstrip(b'\x00')) if strip_nul else 0
    if nul: data = data[:-nul]
    if TEXT_ENCODING == 'vi1':
        text = ''.join(VI_REV.get(b, chr(b) if b < 128 else chr(0xDC00+b)) for b in data)
    else:
        pieces=[]; pos=0
        while pos<len(data):
            b=data[pos]
            if b<128: pieces.append(chr(b));pos+=1;continue
            pair=data[pos:pos+2]
            try:
                decoded=pair.decode('cp949')
                if len(pair)!=2 or decoded.encode('cp949')!=pair: raise ValueError('CP949 alias')
                pieces.append(decoded);pos+=2
            except (UnicodeError,ValueError):
                pieces.append(chr(0xDC00+b));pos+=1
        text=''.join(pieces)
    out = []
    for ch in text:
        v = ord(ch)
        if ch == '\\': out.append('\\\\')
        elif ch == '\n': out.append('\\n')
        elif ch == '\r': out.append('\\r')
        elif ch == '\t': out.append('\\t')
        elif 0xDC80 <= v <= 0xDCFF: out.append('\\x%02X' % (v-0xDC00))
        elif v < 32 or v == 127: out.append('\\x%02X' % v)
        else: out.append(ch)
    return ''.join(out), nul


def edit_text_to_bytes(text: str, replaced=None) -> bytes:
    encoding = WRITE_ENCODING or TEXT_ENCODING
    if encoding == 'vi1': text = unicodedata.normalize('NFC', text)
    out = bytearray(); i = 0
    while i < len(text):
        ch = text[i]
        if ch == '\\' and i+1 < len(text):
            nxt = text[i+1]
            if nxt == 'x' and re.fullmatch('[0-9a-fA-F]{2}', text[i+2:i+4]):
                out.append(int(text[i+2:i+4],16)); i+=4; continue
            if nxt in {'n','r','t','\\'}:
                out.extend({'n':b'\n','r':b'\r','t':b'\t','\\':b'\\'}[nxt]); i+=2; continue
        try:
            if encoding == 'vi1':
                if ch in VI_MAP: out.append(VI_MAP[ch])
                elif ord(ch) < 128: out.append(ord(ch))
                else: raise ValueError('Ngoài bảng VI1: %r' % ch)
            else: out.extend(ch.encode('cp949'))
        except (UnicodeEncodeError, ValueError):
            if ENCODE_ERRORS != 'replace':
                raise ValueError('Ký tự %r không có trong %s. Không tự thay ?; dùng --unknown replace nếu muốn.' % (ch,encoding))
            out.append(63)
            if replaced is not None: replaced.append(ch)
        i+=1
    return bytes(out)


def zt1_decompress(blob: bytes) -> bytes:
    if len(blob) < 8: raise ValueError('ZT1 quá ngắn')
    total, size = struct.unpack_from('<II',blob)
    if total != len(blob): raise ValueError('ZT1: sai kích thước đóng gói')
    dec=zlib.decompressobj(); raw=dec.decompress(blob[8:])+dec.flush()
    if not dec.eof or dec.unused_data or len(raw)!=size: raise ValueError('ZT1: sai zlib/kích thước raw')
    return raw


def zt1_compress(raw: bytes) -> bytes:
    comp=zlib.compress(raw,9)
    return struct.pack('<II',len(comp)+8,len(raw))+comp


def is_gst(raw: bytes) -> bool:
    if len(raw) < 8 or raw[:4] != b'GST\x01':
        return False
    count = struct.unpack_from('<H', raw, 4)[0]
    start = struct.unpack_from('<H', raw, 6)[0]
    if count == 0 or start != 8 + count * 2 or start > len(raw):
        return False
    last = start
    for i in range(count):
        off = struct.unpack_from('<H', raw, 8 + i * 2)[0]
        if off < last or off > len(raw):
            return False
        last = off
    return True


def is_table32(raw: bytes) -> bool:
    if len(raw) < 8:
        return False
    count = struct.unpack_from('<I', raw, 0)[0]
    if count == 0 or count > 200000:
        return False
    table_end = 4 + count * 4
    if table_end > len(raw):
        return False
    last = table_end
    for i in range(count):
        off = struct.unpack_from('<I', raw, 4 + i * 4)[0]
        if off < table_end or off > len(raw) or off < last:
            return False
        last = off
    return True


def classify_zt1(raw: bytes) -> str:
    if is_gst(raw): return 'GST_REPOINT'
    if is_table32(raw): return 'TABLE32_REPOINT'
    if raw[:4] == b'GXL\x01': return 'GXL_RAW'
    return 'RAW_SCAN'


def parse_table32(raw: bytes) -> List[bytes]:
    count = struct.unpack_from('<I', raw, 0)[0]
    offs = [struct.unpack_from('<I', raw, 4 + i * 4)[0] for i in range(count)]
    return [raw[off:(offs[i+1] if i + 1 < count else len(raw))] for i, off in enumerate(offs)]


def rebuild_table32(items: List[bytes]) -> bytes:
    count = len(items)
    pos = 4 + count * 4
    offs = []
    for s in items:
        offs.append(pos)
        pos += len(s)
    return struct.pack('<I', count) + b''.join(struct.pack('<I', x) for x in offs) + b''.join(items)


def parse_gst(raw: bytes) -> List[bytes]:
    count = struct.unpack_from('<H', raw, 4)[0]
    start = struct.unpack_from('<H', raw, 6)[0]
    items = []
    last = start
    for i in range(count):
        end = struct.unpack_from('<H', raw, 8 + i * 2)[0]
        items.append(raw[last:end])
        last = end
    return items


def rebuild_gst(items: List[bytes]) -> bytes:
    count = len(items)
    start = 8 + count * 2
    pos = start
    ends = []
    for s in items:
        pos += len(s)
        if pos > 0xFFFF:
            raise ValueError('GST sau khi dịch dài 0x%X, vượt giới hạn 0xFFFF' % pos)
        ends.append(pos)
    return b'GST\x01' + struct.pack('<HH', count, start) + b''.join(struct.pack('<H', x) for x in ends) + b''.join(items)


def scan_raw_strings(raw: bytes, min_len: int = 3) -> List[Tuple[int, bytes]]:
    out = []
    i = 0
    while i < len(raw):
        if 0x20 <= raw[i] <= 0x7E:
            j = i + 1
            while j < len(raw) and 0x20 <= raw[j] <= 0x7E:
                j += 1
            s = raw[i:j]
            if len(s) >= min_len and re.search(rb'[A-Za-z]', s):
                out.append((i, s))
            i = j
        else:
            i += 1
    return out



def write_direct_txt(path: Path, header: List[str], recs: List[Dict[str, Any]]) -> None:
    """Ghi text dạng sửa trực tiếp: mỗi record là 1 dòng, không dùng src/dst JSON."""
    ensure_dir(path.parent)
    with path.open('w', encoding='utf-8', newline='\n') as f:
        for h in header:
            f.write('# ' + h + '\n')
        f.write('# FORMAT: [index|field=value|flag] sửa chữ trực tiếp sau dấu ]\n')
        f.write('# Lưu ý: giữ nguyên dấu \\n, \\r, \\t, \\xHH nếu có. Không xóa dòng [index|...]\n')
        f.write('# Encoding theo manifest; ký tự không biểu diễn được sẽ báo lỗi mặc định.\n')
        for r in recs:
            idx = int(r.get('index', 0))
            typ = r.get('type', '')
            text = r.get('src', r.get('text', ''))
            fields = [f'{idx:06d}']
            if 'nul' in r: fields.append('nul=%s' % r.get('nul'))
            if 'raw_len' in r: fields.append('len=%s' % r.get('raw_len'))
            if 'offset' in r: fields.append('off=0x%X' % int(r.get('offset', 0)))
            if 'max_len' in r: fields.append('max=%s' % r.get('max_len'))
            if 'va' in r: fields.append('va=0x%X' % r['va'])
            if 'section' in r: fields.append('section=%s' % r['section'])
            if typ in ('GXL_RAW', 'RAW_SCAN', 'SO_RAW', 'BINARY_RAW','BINARY_FIXED_RAW','GUI_FIXED_RAW'): fields.append('RAW')
            if r.get('fixed_safe'): fields.append('FIXED_SLOT')
            f.write('[%s] %s\n' % ('|'.join(fields), text))


def _parse_header_value(lines: List[str], key: str) -> str:
    prefix = '# ' + key + '='
    for line in lines:
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return ''


def read_direct_txt(path: Path) -> List[Dict[str, Any]]:
    """Đọc file dump mới. Vẫn hỗ trợ JSONL cũ để tránh hỏng dump cũ."""
    lines = path.read_text(encoding='utf-8').splitlines()
    path_val = _parse_header_value(lines, 'path')
    type_val = _parse_header_value(lines, 'type')
    recs: List[Dict[str, Any]] = []
    rx = re.compile(r'^\[(\d+)([^\]]*)\]\s?(.*)$')
    for lineno, line in enumerate(lines, 1):
        if not line or line.startswith('#'):
            continue
        # Tương thích dump JSON cũ
        if line.startswith('{'):
            r = json.loads(line)
            r['text'] = r.get('dst', r.get('src', ''))
            r['_txt_file'] = str(path); r['_line'] = lineno
            recs.append(r)
            continue
        m = rx.match(line)
        if not m:
            raise RuntimeError('Sai format %s dòng %d: %s' % (path, lineno, line[:120]))
        idx = int(m.group(1))
        rest = m.group(2).strip()
        text = m.group(3)
        fields: Dict[str, Any] = {}
        flags: List[str] = []
        if rest.startswith('|'):
            rest = rest[1:]
        if rest:
            for part in rest.split('|'):
                if not part:
                    continue
                if '=' in part:
                    k, v = part.split('=', 1)
                    fields[k] = v
                else:
                    flags.append(part)
        kind = 'so' if type_val == 'SO_RAW' else 'zt1'
        r: Dict[str, Any] = {'kind': kind, 'path': path_val, 'type': type_val, 'index': idx, 'text': text, '_txt_file': str(path), '_line': lineno}
        if 'nul' in fields: r['nul'] = int(fields['nul'], 0)
        if 'len' in fields: r['raw_len'] = int(fields['len'], 0)
        if 'off' in fields: r['offset'] = int(fields['off'], 0)
        if 'max' in fields: r['max_len'] = int(fields['max'], 0)
        if 'RAW' in flags: r['note'] = 'RAW_KHONG_REPOINT'
        recs.append(r)
    return recs



def dump_graphic(z: zipfile.ZipFile, asset: str, out_dir: Path, rows: List[Dict[str, Any]], log: List[str]) -> None:
    try:
        blob = z.read(asset)
    except KeyError:
        log.append('[graphic][MISS] ' + asset); return
    stem = safe_name(asset.rsplit('.',1)[0])
    raw_dir = out_dir / '_raw_pzx'; ensure_dir(raw_dir)
    (raw_dir / (stem + '.pzx')).write_bytes(blob)
    item_dir = out_dir / stem; ensure_dir(item_dir)
    if not HAS_PZX:
        log.append('[graphic][RAW] %s: thiếu pzx2png_all.py/Pillow, chỉ copy raw PZX' % asset); return
    try:
        images, meta = parse_pzx(blob)
        decoded = meta.get('decoded_images', [])
        for i, im in enumerate(images):
            fn = '%s_%03d.png' % (stem, i)
            im.save(item_dir / fn)
            imeta = decoded[i] if i < len(decoded) else {}
            rows.append({'source':asset,'index':i,'png':str((item_dir/fn).relative_to(out_dir)).replace('\\','/'),'width':im.width,'height':im.height,'variant':meta.get('variant',''),'image_type':meta.get('image_type',''),'mode':imeta.get('mode',''),'palette_count':imeta.get('palette_count',''),'offset':hex(imeta.get('offset',0)) if imeta else ''})
        make_atlas_labeled(images).save(item_dir / (stem + '_atlas_labeled.png'))
        log.append('[graphic] %s: %d ảnh' % (asset, len(images)))
    except Exception as e:
        log.append('[graphic][ERR] %s: %s' % (asset, e))



# WIPI container support. DLG never enters the ZT1 compression path.
class GameInput:
    def __init__(self, path):
        self.path=Path(path).resolve(); self.z=None
        if self.path.is_dir():
            self.root=self.path/'nitrofs' if (self.path/'nitrofs'/'binary.mod').is_file() else self.path
            if not (self.root/'binary.mod').is_file():
                raise ValueError('Chọn thư mục dự án có nitrofs/binary.mod hoặc chính thư mục nitrofs.')
            self.names=sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file())
        elif zipfile.is_zipfile(self.path):
            self.z=zipfile.ZipFile(self.path)
            infos=self.z.infolist(); self.all_names=[i.filename for i in infos]
            if len(self.all_names)!=len(set(self.all_names)): raise ValueError('ZIP có tên trùng.')
            for n in self.all_names:
                safe_relative(n)
                info=self.z.getinfo(n)
                if (info.external_attr>>16)&0o170000==0o120000: raise ValueError('Không nhận symlink trong ZIP.')
            roots=[n[:-len('binary.mod')] for n in self.all_names if n=='binary.mod' or n.endswith('/nitrofs/binary.mod') or n=='nitrofs/binary.mod']
            if len(roots)!=1: raise ValueError('ZIP phải có đúng một nitrofs/binary.mod (hoặc binary.mod ở gốc).')
            self.prefix=roots[0]
            self.names=sorted(n[len(self.prefix):] for n in self.all_names if n.startswith(self.prefix) and not n.endswith('/'))
        else: raise ValueError('Chỉ hỗ trợ thư mục WIPI/DSi hoặc ZIP nguồn; không sửa trực tiếp .nds/.apk.')
    def read(self,name):
        safe_relative(name)
        if name not in self.names: raise ValueError('Không có tài nguyên: '+name)
        return self.z.read(self.prefix+name) if self.z else (self.root/name).read_bytes()
    def close(self):
        if self.z: self.z.close()
    def __enter__(self): return self
    def __exit__(self,*args): self.close()


def safe_relative(name):
    from pathlib import PurePosixPath
    p=PurePosixPath(name)
    if not name or '\\' in name or ':' in name or '\x00' in name or p.is_absolute() or '..' in p.parts:
        raise ValueError('Đường dẫn không an toàn: '+repr(name))
    return p


def sha(data): return hashlib.sha256(data).hexdigest()


def binary_text_ranges(blob):
    """ELF32 little-endian PROGBITS/ALLOC only. Code/data may coexist: these are candidates."""
    if len(blob)<52 or blob[:6]!=b'\x7fELF\x01\x01': raise ValueError('binary.mod phải là ELF32 little-endian')
    shoff=struct.unpack_from('<I',blob,32)[0]
    entsize,num,shstridx=struct.unpack_from('<HHH',blob,46)
    if entsize<40 or not num or shstridx>=num or shoff+entsize*num>len(blob): raise ValueError('ELF section table không hợp lệ')
    headers=[struct.unpack_from('<10I',blob,shoff+i*entsize) for i in range(num)]
    strings=headers[shstridx];start,size=strings[4:6]
    if start+size>len(blob): raise ValueError('ELF shstrtab vượt file')
    names=blob[start:start+size];ranges=[]
    for h in headers:
        name_off,typ,flags,va,offset,length=h[:6]
        if typ!=1 or not(flags&2):continue
        if offset+length>len(blob) or name_off>=len(names):raise ValueError('ELF section vượt file')
        name=names[name_off:].split(b'\0',1)[0].decode('ascii',errors='replace')
        ranges.append((name,offset,length,va))
    if not ranges:raise ValueError('Không tìm thấy section dữ liệu ELF')
    return ranges


def binary_records(blob):
    candidates=[]
    for section,start,length,va in binary_text_ranges(blob):
        data=blob[start:start+length];found={};cursor=0
        # Prefer complete NUL-terminated CP949/ASCII strings. Never decode relocation or BSS.
        for part in data.split(b'\0')[:-1]:
            off=cursor;cursor+=len(part)+1
            if not 3<=len(part)<=4096:continue
            try:text=part.decode('cp949')
            except UnicodeDecodeError:continue
            if not all(ch.isprintable() or ch in '\r\n\t' for ch in text):continue
            letters=sum(ch.isascii() and ch.isalpha() for ch in text)
            hangul=sum('\uac00'<=ch<='\ud7a3' or '\u3130'<=ch<='\u318f' for ch in text)
            if letters>=3 or hangul>=2:found[off]=part
        # ASCII suffixes can follow binary alignment/type bytes; keep only NUL-terminated runs.
        for off,part in scan_raw_strings(data,2):
            if not re.search(rb'[A-Za-z]{3}',part) and part not in (b'HP',b'MP',b'SP',b'LV',b'Lv',b'OK',b'ON',b'NO',b'ID'): continue
            end=off+len(part)
            if end>=len(data) or data[end]!=0:continue
            if any(a<=off and end<=a+len(b) for a,b in found.items()):continue
            found[off]=part
        for off,part in sorted(found.items()):
            text,_=bytes_to_edit_text(part)
            candidates.append(dict(kind='raw',path='binary.mod',type='BINARY_RAW',index=len(candidates),
                offset=start+off,max_len=len(part),section=section,va=va+off,src=text,note='RAW_KHONG_REPOINT'))
    return 'BINARY_RAW',candidates



# v1.3: verified text-slot maps for WIPI native-top v041, including r1/r2 English.
# Masked hashes validate ALL non-text bytes; never trust offsets from a TXT file.
FIXED_CATALOG = {'binary.mod': {'slots': [(526904, 69), (527164, 68), (527236, 34), (527412, 18), (527512, 12), (527528, 10), (527540, 10), (528020, 5), (528028, 6), (528048, 52), (528104, 43), (528148, 41), (528208, 5), (528216, 35), (528260, 10), (528272, 8), (528284, 10), (528296, 19), (528316, 15), (528332, 47), (528380, 69), (528468, 18), (528488, 21), (528512, 14), (528528, 14), (528552, 12), (528568, 9), (528580, 9), (528592, 20), (530800, 68), (532805, 6), (532980, 12), (532996, 16), (534068, 14), (534084, 4), (534092, 4), (534100, 4), (534539, 6), (534548, 6), (534557, 6), (534566, 6), (534575, 6), (534584, 6), (534593, 4), (534602, 5), (534611, 8), (534620, 8), (534638, 8), (534924, 18), (534944, 28), (534976, 19), (535012, 16), (535092, 34), (535192, 10), (535204, 10), (535216, 8), (535228, 6), (535236, 4), (535244, 6), (535252, 6), (535260, 10), (535272, 8), (535284, 8), (535296, 6), (535304, 6), (535312, 6), (535320, 6), (535328, 6), (535336, 8), (535348, 8), (535360, 8), (535372, 8), (535384, 8), (535396, 8), (535408, 8), (535420, 8), (535432, 8), (535444, 8), (535456, 8), (535468, 6), (535476, 6), (535484, 6), (535492, 6), (535500, 4), (535508, 4), (535516, 4), (535524, 4), (535532, 4), (535540, 4), (535548, 4), (535556, 4), (535564, 8), (535576, 4), (535584, 6), (535592, 6), (535600, 8), (535612, 6), (535920, 4), (535980, 13), (535996, 16), (536016, 13), (536032, 14), (536048, 17), (536076, 10), (536088, 17), (536108, 9), (536120, 9), (536132, 9), (536144, 9), (536156, 9), (536168, 8), (536180, 9), (536192, 9), (536204, 9), (536216, 9), (536228, 8), (536240, 9), (536252, 9), (536264, 9), (536276, 9), (536286, 4), (536295, 4), (536304, 4), (536313, 4), (536322, 4), (536331, 8), (536340, 6), (536349, 4), (536358, 4), (536367, 4), (536376, 4), (536385, 4), (536394, 6), (536403, 4), (536410, 4), (536417, 4), (536424, 4), (536431, 4), (536438, 6), (536445, 6), (536452, 4), (536459, 4), (536466, 6), (536473, 6), (536480, 6), (536487, 6), (536656, 37), (536868, 4), (536920, 6), (536934, 6), (537004, 6), (537011, 6), (537018, 6), (537025, 6), (537032, 8), (537041, 8), (537050, 6), (537059, 8), (537074, 8), (537083, 6), (537092, 6), (537101, 6), (537110, 4), (537119, 6), (537128, 6), (537137, 8), (537146, 4), (537155, 6), (537164, 8), (537173, 8), (537182, 8), (537191, 8), (537200, 8), (537209, 8), (537240, 63), (537376, 20), (537436, 65), (537560, 6), (537567, 5), (537574, 5), (537581, 6), (537588, 4), (537595, 4), (537604, 4), (537613, 6), (537622, 6), (537631, 8), (537640, 4), (537649, 4), (537658, 4), (537667, 4), (537676, 4), (537685, 4), (537694, 8), (537703, 6), (537712, 4), (537721, 4), (537730, 4), (537739, 4), (537748, 4), (537757, 6), (537766, 4), (537775, 4), (537784, 4), (537793, 6), (537802, 8), (537912, 140), (538056, 21), (538132, 4), (538139, 4), (538146, 4), (538153, 4), (538160, 4), (538167, 6), (538174, 6), (538181, 4), (538188, 4), (538196, 11), (538208, 11), (538220, 13), (538236, 11), (538336, 6), (538344, 16), (538364, 5), (538372, 5), (538380, 5), (538440, 8), (538452, 12), (538516, 32), (538620, 13), (539672, 8), (539728, 4), (539737, 4), (539746, 4), (539755, 4), (539764, 4), (539773, 8), (539782, 6), (539791, 4), (539800, 4), (539809, 4), (539818, 4), (539827, 4), (539836, 6), (539928, 4), (539935, 6), (539942, 4), (539949, 4), (540116, 30), (540148, 19), (540180, 26), (540232, 6), (540393, 4), (540402, 4), (540411, 4), (540420, 4), (540429, 8), (540438, 6), (540447, 4), (540456, 4), (540465, 4), (540474, 4), (540483, 4), (540492, 6), (540504, 10), (540516, 10), (540528, 8), (540540, 6), (540548, 4), (540556, 6), (540564, 6), (540572, 10), (540584, 8), (540596, 8), (540608, 6), (540616, 6), (540624, 6), (540632, 6), (540640, 6), (540648, 8), (540660, 8), (540672, 8), (540684, 8), (540696, 8), (540708, 8), (540720, 8), (540732, 8), (540744, 8), (540756, 8), (540768, 8), (540780, 6), (540788, 6), (540796, 6), (540804, 6), (540812, 4), (540820, 4), (540828, 4), (540836, 4), (540936, 4), (540944, 4), (540952, 4), (540960, 4), (540968, 6), (540992, 9), (541004, 11), (541016, 8), (541040, 26), (541068, 11), (541080, 16), (541100, 7), (541160, 17), (541180, 19), (541200, 39), (541240, 12), (543488, 20), (546103, 4), (546110, 6), (546117, 4), (546124, 4), (548621, 4), (548626, 4), (548631, 4), (548708, 22), (550076, 6), (550176, 29), (550208, 137), (550348, 95), (550748, 8), (550757, 8), (550768, 6), (550776, 6), (550784, 6), (550792, 11), (550804, 13), (550820, 13), (550836, 13), (550852, 7), (551184, 11), (551196, 27), (551240, 8), (551252, 6), (551260, 8), (551272, 62), (551336, 27), (551364, 4), (551420, 23), (551444, 8), (551720, 13), (551816, 67), (551948, 24), (551976, 36), (552016, 36), (552072, 17), (552092, 17), (552112, 4), (552120, 4), (552148, 21), (552172, 4), (552180, 4), (552204, 21), (552240, 11), (552252, 11), (552264, 13), (552280, 11), (552384, 6), (552392, 6), (552400, 6), (552408, 4), (552416, 4), (552424, 8), (552436, 4), (552444, 10), (552456, 10), (552468, 10), (552480, 6), (552488, 8), (552500, 6), (552508, 6), (552516, 10), (552528, 10), (552540, 6), (552548, 6), (552556, 12), (552572, 12), (552588, 14), (552604, 12), (552620, 14), (552636, 14), (552652, 12), (552668, 14), (552684, 9), (552696, 12), (552712, 14), (552728, 4), (552736, 4), (552744, 6), (552752, 8), (552764, 4), (552772, 4), (552780, 8), (552792, 4), (552800, 4), (552808, 8), (552820, 10), (552832, 6), (552840, 8), (552852, 8), (552864, 8), (552876, 8), (552888, 6), (552896, 6), (552904, 4), (552912, 4), (552920, 4), (552928, 4), (552936, 4), (552944, 6), (552952, 4), (552960, 8), (552972, 6), (552980, 6), (552996, 14), (553012, 13), (553052, 13), (553068, 8), (553100, 8), (555784, 15), (555800, 11), (555812, 5), (563136, 25), (563164, 25), (563192, 21), (563216, 28), (563248, 29), (563280, 23), (563304, 30), (563336, 29), (563368, 32), (563404, 32), (563440, 32), (563476, 36), (563516, 30), (563548, 35), (563584, 32), (563620, 22), (563644, 33), (563680, 32), (563716, 30), (563748, 35), (563784, 30), (563816, 32), (563852, 32), (563888, 21), (563912, 33), (563948, 20), (563996, 27), (564024, 26), (564052, 30), (564084, 25), (564112, 19), (564132, 21), (564156, 23), (564180, 26), (564208, 29), (564240, 27), (564268, 21), (570224, 60), (699448, 6), (699455, 6), (699462, 6), (700268, 8), (700278, 8), (700288, 8), (700298, 6), (700308, 8), (700318, 8), (537231, 2), (537234, 2), (537237, 2), (552987, 2), (552990, 2), (552993, 2)], 'masked_sha256': 'fa157dee61465d750b6719ec24b28bd54f9e5667470196797bebd26387d32a96'}, 'menu/Title.gui': {'slots': [(180, 9), (483, 17), (509, 17), (608, 11), (628, 20), (657, 15), (692, 19), (907, 19), (720, 8), (935, 8), (821, 11), (841, 20), (870, 17)], 'masked_sha256': '56839832d89849fe9d151eaf9e02a1bc3294448d2da1d7ca2bb9351a5aec25a7'}}

def verified_fixed_records(path, blob):
    entry=FIXED_CATALOG[path]; spans=entry['slots']; masked=bytearray(blob)
    for off,size in spans:
        if off+size>=len(blob) or blob[off+size]!=0:return None
        masked[off:off+size]=b'\0'*size
    if sha(masked)!=entry['masked_sha256']:return None
    typ='BINARY_FIXED_RAW' if path=='binary.mod' else 'GUI_FIXED_RAW'
    recs=[]
    for off,size in spans:
        raw=blob[off:off+size];body=raw.split(b'\0',1)[0] if path=='binary.mod' else raw.rstrip(b' ')
        text,_=bytes_to_edit_text(body)
        recs.append(dict(kind='raw',path=path,type=typ,index=len(recs),offset=off,max_len=size,src=text,fixed_safe=True))
    return typ,recs


def dump_text_name(path,typ):
    return 'txt/'+safe_name(path)+('' if typ in ('TABLE32_REPOINT','GST_REPOINT') else '_raw')+'.txt'


def resource_records(path, blob):
    if path in FIXED_CATALOG:
        fixed=verified_fixed_records(path,blob)
        if fixed:
            typ,recs=fixed
            if path=='binary.mod':
                spans=FIXED_CATALOG[path]['slots']
                for r in binary_records(blob)[1]:
                    if any(r['offset'] < off+n and off < r['offset']+r['max_len'] for off,n in spans):continue
                    r=dict(r,type=typ,index=len(recs));recs.append(r)
            return typ,recs
        if path=='binary.mod':return binary_records(blob)
        raise ValueError('Title.gui không khớp layout v041 đã xác minh; không đoán offset.')
    is_dlg=path.lower().endswith('.dlg')
    raw=blob if is_dlg else zt1_decompress(blob)
    typ=classify_zt1(raw)
    if is_dlg and typ!='TABLE32_REPOINT': raise ValueError('DLG không phải bảng offset32 đã xác minh; dừng, không đoán định dạng.')
    if typ=='TABLE32_REPOINT':
        items=parse_table32(raw)
    elif typ=='GST_REPOINT':
        items=parse_gst(raw)
    else:
        recs=[]
        for idx,(off,b) in enumerate(scan_raw_strings(raw,2 if typ=='GXL_RAW' else 4)):
            text,_=bytes_to_edit_text(b)
            recs.append(dict(kind='raw',path=path,type=typ,index=idx,offset=off,max_len=len(b),src=text))
        return typ,recs
    recs=[]
    for idx,b in enumerate(items):
        text,nul=bytes_to_edit_text(b,True)
        recs.append(dict(kind='dlg' if is_dlg else 'zt1',path=path,type=typ,index=idx,raw_len=len(b),nul=nul,src=text))
    return typ,recs


def export_game(source, output, encoding='cp949', graphics=True):
    global TEXT_ENCODING
    TEXT_ENCODING=encoding
    output=Path(output).resolve()
    if output.exists(): raise ValueError('Thư mục dump đã tồn tại; chọn thư mục mới để không mất bản dịch.')
    with GameInput(source) as game:
        output.mkdir(parents=True); (output/'txt').mkdir()
        manifest=dict(tool='zenonia-wipi-aio',version=1,encoding=encoding,files=[],binary_sha256=sha(game.read('binary.mod')))
        log=[]; rows=[]
        try:
            for name in game.names:
                if name!='binary.mod' and name!='menu/Title.gui' and not name.lower().endswith(('.dlg','.zt1')): continue
                blob=game.read(name); typ,recs=resource_records(name,blob)
                fn=dump_text_name(name,typ)
                if any(e['txt']==fn for e in manifest['files']): raise ValueError('Trùng tên dump: '+name)
                write_direct_txt(output/fn,[
                    'Zenonia WIPI/DSi - sửa trực tiếp nội dung sau ]',
                    'path='+name,'type='+typ,'encoding='+encoding,
                    'DLG=RAW, tuyệt đối không nén; ZT1=zlib với header 8 byte.',
                    'Mã hiển thị !C, !N, !cRRGGBB được phép đổi và sẽ ghi cảnh báo. Giữ escape.',
                    'File _raw.txt KHÔNG REPOINT. Chỉ dòng FIXED_SLOT được sửa, không quá max byte; RAW khác chỉ tham khảo.',
                    'Detail: L/M/H; chuỗi GUI cũng là chữ render. Không tự đổi ảnh/font hoặc ánh xạ phím.'
                ],recs)
                manifest['files'].append(dict(path=name,txt=fn,type=typ,records=len(recs),sha256=sha(blob),storage='elf-binary-raw' if name=='binary.mod' else 'raw-dlg' if name.endswith('.dlg') else 'zt1-zlib'))
                log.append('%s: %s, %d chuỗi' % (name,typ,len(recs)))
            (output/'font_reference').mkdir()
            for name in game.names:
                if name.lower().endswith('.ft2'):
                    (output/'font_reference'/Path(name).name).write_bytes(game.read(name))
            (output/'_manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
            if graphics:
                for name in ['menu/Title.pzx','menu/Title_1.pzx','button.pzx']:
                    if name in game.names: dump_graphic(game,name,output/'graphic',rows,log)
                if rows:
                    with (output/'graphic/_graphic_manifest.csv').open('w',newline='',encoding='utf-8') as f:
                        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
            (output/'_dump_log.txt').write_text('\n'.join(log)+'\n',encoding='utf-8')
        except Exception:
            (output/'_INCOMPLETE.txt').write_text('Xuất lỗi, không dùng dump này để import.\n',encoding='utf-8'); raise
    print('Đã xuất %d file text -> %s' % (len(manifest['files']),output))
    if encoding=='vi1': print('VI1 chỉ mã hóa dữ liệu, CHƯA patch renderer/font native cho tiếng Việt.')


def rebuild_resource(name, blob, records, typ, log):
    original_type, originals=binary_records(blob) if typ=='BINARY_RAW' and name=='binary.mod' else resource_records(name,blob)
    if original_type!=typ: raise ValueError('Sai kiểu: '+name)
    if len(records)!=len(originals) or {r['index'] for r in records}!=set(range(len(originals))):
        raise ValueError('Thiếu/thừa/trùng index trong '+name)
    editable=typ in ('TABLE32_REPOINT','GST_REPOINT')
    raw=blob if name=='binary.mod' or name.endswith('.gui') or name.lower().endswith('.dlg') else zt1_decompress(blob)
    fixed_buffer=bytearray(raw)
    items=(parse_table32(raw) if typ=='TABLE32_REPOINT' else parse_gst(raw)) if editable else None
    changed=0
    for r in records:
        idx=r['index']; orig=originals[idx]
        if r.get('path')!=name or r.get('type')!=typ: raise ValueError('Header không khớp '+name)
        if r['text']==orig['src']: continue  # Preserve original byte sequences, even noncanonical CP949.
        if not editable:
            if not orig.get('fixed_safe'):raise ValueError('RAW/GXL chưa xác minh, chỉ tham khảo: %s index=%d'%(name,idx))
            off=orig['offset'];cap=orig['max_len']
            if r.get('offset')!=off or r.get('max_len')!=cap:raise ValueError('Không sửa metadata off/max: %s index=%d'%(name,idx))
            data=edit_text_to_bytes(r['text'])
            if b'\0' in data:raise ValueError('Không chèn NUL vào chuỗi fixed slot')
            if len(data)>cap:raise ValueError('Vượt giới hạn byte: %s index=%d cần=%d tối đa=%d; không repoint'%(name,idx,len(data),cap))
            fmt=rb'%(?:[-+ #0]*\d*(?:\.\d+)?[hlL]?)[diuoxXfFeEgGcsp%%]'
            before=raw[off:off+cap].split(b'\0',1)[0]
            if re.findall(fmt,before)!=re.findall(fmt,data):raise ValueError('Không đổi tham số printf của fixed slot')
            # Fixed slots require exact control codes; only prose may change.
            cmd=rb'!c[0-9A-Fa-f]{6}|![A-Z]'
            if re.findall(cmd,before)!=re.findall(cmd,data):raise ValueError('Không đổi mã điều khiển của fixed slot')
            for match in re.finditer(rb'!c',data):
                if not re.match(rb'!c[0-9A-Fa-f]{6}',data[match.start():]):raise ValueError('Mã màu fixed slot không hợp lệ')
            pad=b' ' if name.endswith('.gui') else b'\0'
            fixed_buffer[off:off+cap]=data+pad*(cap-len(data));changed+=1
            continue
        if r.get('nul')!=orig['nul']: raise ValueError('Không sửa metadata nul: %s index=%d' % (name,idx))
        # Android localization changes pagination/colors. Permit known display commands,
        # but retain strict validation for unknown opcode differences and malformed colors.
        token_re=r'!c[0-9A-Fa-f]{6}|![A-Z]'
        old_tokens=re.findall(token_re,orig['src']);new_tokens=re.findall(token_re,r['text'])
        for color in re.finditer(r'!c',r['text']):
            if not re.match(r'!c[0-9A-Fa-f]{6}',r['text'][color.start():]):
                raise ValueError('Mã màu !c sai định dạng: %s index=%d'%(name,idx))
        if new_tokens!=old_tokens:
            known=lambda t: t in ('!C','!N') or re.fullmatch(r'!c[0-9A-Fa-f]{6}',t)
            if CONTROL_POLICY=='strict' or [t for t in old_tokens if not known(t)]!=[t for t in new_tokens if not known(t)]:
                raise ValueError('Mã điều khiển bị thay đổi: %s index=%d (strict hoặc mã chưa hỗ trợ)'%(name,idx))
            log.append('[CTRL][WARN] %s index=%d: %s -> %s; giữ mã của bản dịch'%(name,idx,old_tokens,new_tokens))
        replaced=[]
        try: new=edit_text_to_bytes(r['text'],replaced)+b'\0'*orig['nul']
        except ValueError as e: raise ValueError('%s index=%d: %s'%(name,idx,e)) from e
        body = new[:-orig['nul']] if orig['nul'] else new
        if b'\0' in body:
            raise ValueError('Không chèn NUL vào giữa chuỗi: %s index=%d'%(name,idx))
        if replaced: log.append('[?] %s #%d: %r'%(name,idx,''.join(dict.fromkeys(replaced))))
        if new!=items[idx]: items[idx]=new;changed+=1
    if not changed: return blob
    if not editable:
        assert len(fixed_buffer)==len(blob)
        log.append('[FIXED] %s changed=%d; giữ độ dài, không repoint'%(name,changed))
        return bytes(fixed_buffer)
    if typ=='TABLE32_REPOINT':
        count=len(items); table_end=4+4*count
        # Retain any padding before the first string; rebuild all absolute file offsets.
        first=struct.unpack_from('<I',raw,4)[0]
        gap=raw[table_end:first]; pos=table_end+len(gap); offsets=[]
        for item in items: offsets.append(pos);pos+=len(item)
        rebuilt=struct.pack('<I',count)+struct.pack('<%dI'%count,*offsets)+gap+b''.join(items)
    else:
        count=len(items); last=struct.unpack_from('<H',raw,8+2*(count-1))[0]
        rebuilt=rebuild_gst(items)+raw[last:]  # Preserve any bytes after the last GST string.
    log.append('[text] %s changed=%d bytes=%d->%d'%(name,changed,len(raw),len(rebuilt)))
    if name.lower().endswith('.dlg'):
        if not is_table32(rebuilt): raise ValueError('DLG sau repoint không hợp lệ')
        log.append('[DLG] RAW offset32, KHÔNG nén zlib / KHÔNG thêm header ZT1')
        return rebuilt
    return zt1_compress(rebuilt)


def import_game(source, dump, output, unknown='strict', target_encoding=None, control_policy='compatible'):
    global TEXT_ENCODING,ENCODE_ERRORS,WRITE_ENCODING,CONTROL_POLICY
    CONTROL_POLICY=control_policy
    if control_policy not in ('compatible','strict'): raise ValueError('Control policy không hợp lệ')
    dump=Path(dump).resolve();output=Path(output).resolve()
    if output.exists(): raise ValueError('Đầu ra đã tồn tại; không ghi đè.')
    if (dump/'_INCOMPLETE.txt').exists(): raise ValueError('Dump xuất chưa hoàn tất.')
    manifest=json.loads((dump/'_manifest.json').read_text(encoding='utf-8'))
    if manifest.get('tool')!='zenonia-wipi-aio' or manifest.get('version')!=1: raise ValueError('Không nhận dump Android/cũ.')
    TEXT_ENCODING=manifest['encoding'];ENCODE_ERRORS=unknown;WRITE_ENCODING=target_encoding
    if target_encoding not in (None,'cp949','vi1'): raise ValueError('Target encoding không hợp lệ')
    if TEXT_ENCODING not in ('cp949','vi1'): raise ValueError('Encoding không hợp lệ')
    log=['WIPI/DSi import v1.3; binary/GUI chỉ sửa slot đã xác minh; font giữ nguyên.'];replacements={};seen=set()
    with GameInput(source) as game:
        if game.z is None:
            # Prevent copying output recursively into its own source or copying dump/build by accident.
            try: output.relative_to(game.path)
            except ValueError: pass
            else: raise ValueError('Đầu ra phải nằm ngoài thư mục nguồn.')
        if sha(game.read('binary.mod'))!=manifest['binary_sha256']: raise ValueError('binary.mod khác bản đã dump.')
        expected={n for n in game.names if n.lower().endswith(('.dlg','.zt1'))}
        for optional in ('binary.mod','menu/Title.gui'):
            if any(entry.get('path')==optional for entry in manifest['files']):expected.add(optional)
        for entry in manifest['files']:
            name=entry['path'];safe_relative(name);safe_relative(entry['txt'])
            if name in seen: raise ValueError('Manifest trùng path')
            seen.add(name);blob=game.read(name)
            if sha(blob)!=entry['sha256']: raise ValueError('Nguồn đã thay đổi: '+name)
            records=read_direct_txt(dump/entry['txt'])
            if len(records)!=entry['records']: raise ValueError('Thay đổi số record: '+name)
            new=rebuild_resource(name,blob,records,entry['type'],log)
            if new!=blob: replacements[name]=new
        if seen!=expected: raise ValueError('Manifest thiếu/thừa file tài nguyên.')
        # All text validates BEFORE any output is created.
        output.parent.mkdir(parents=True,exist_ok=True)
        if game.z:
            if output.suffix.lower()!='.zip': raise ValueError('Nguồn ZIP cần đầu ra .zip')
            fd,tmp=tempfile.mkstemp(prefix='.wipi-',suffix='.zip',dir=output.parent);os.close(fd)
            try:
                with zipfile.ZipFile(tmp,'w') as out:
                    for info in game.z.infolist():
                        relative=info.filename[len(game.prefix):] if info.filename.startswith(game.prefix) else None
                        out.writestr(copy_zipinfo(info),replacements.get(relative,game.z.read(info.filename)))
                os.replace(tmp,output)
            finally:
                if os.path.exists(tmp): os.unlink(tmp)
        else:
            temp=Path(tempfile.mkdtemp(prefix='.wipi-',dir=output.parent))
            try:
                shutil.copytree(game.path,temp/'game',symlinks=True)
                target=(temp/'game'/'nitrofs') if game.root!=game.path else temp/'game'
                for name,blob in replacements.items():
                    p=target/name
                    if p.is_symlink() or any(q.is_symlink() for q in p.parents if q!=temp.parent): raise ValueError('Không ghi đè symlink')
                    p.write_bytes(blob)
                os.replace(temp/'game',output)
            finally: shutil.rmtree(temp,ignore_errors=True)
    if (WRITE_ENCODING or TEXT_ENCODING)=='vi1': log.append('[WARN] VI1 không tự patch binary.mod hay font. Cần renderer native hỗ trợ trước khi chữ Việt có dấu hiển thị đúng.')
    log.append('Đã thay %d tài nguyên; cần build lại .nds từ dự án đầu ra.'%len(replacements))
    output.with_name(output.name+'.report.txt').write_text('\n'.join(log)+'\n',encoding='utf-8')
    control_warnings=[line for line in log if line.startswith('[CTRL][WARN]')]
    console=[line for line in log if not line.startswith('[CTRL][WARN]')]
    if control_warnings: console.append('[CTRL] %d chuỗi đổi mã hiển thị; xem chi tiết trong .report.txt'%len(control_warnings))
    print('\n'.join(console)); print('Đầu ra:',output)


def transfer_android_text(android_txt, wipi_dump, output_dump):
    """Copy prose by matching indices; retain WIPI headers/metadata and the manifest."""
    wipi_dump=Path(wipi_dump).resolve();output_dump=Path(output_dump).resolve()
    if output_dump.exists(): raise ValueError('Dump đầu ra đã tồn tại; chọn thư mục mới.')
    try: output_dump.relative_to(wipi_dump)
    except ValueError: pass
    else: raise ValueError('Dump đầu ra phải nằm ngoài dump nguồn.')
    if (wipi_dump/'_INCOMPLETE.txt').exists(): raise ValueError('Dump WIPI chưa hoàn tất.')
    manifest=json.loads((wipi_dump/'_manifest.json').read_text(encoding='utf-8'))
    if manifest.get('tool')!='zenonia-wipi-aio' or manifest.get('version')!=1: raise ValueError('Cần dump WIPI có manifest hợp lệ.')
    matches=[e for e in manifest['files'] if e['path']=='script/output.dlg']
    if len(matches)!=1: raise ValueError('Không tìm thấy duy nhất script/output.dlg trong manifest.')
    entry=matches[0];safe_relative(entry['txt'])
    original=read_direct_txt(wipi_dump/entry['txt']);incoming=read_direct_txt(Path(android_txt))
    expected=set(range(entry['records']))
    if any(len(recs)!=entry['records'] or {r['index'] for r in recs}!=expected for recs in (original,incoming)):
        raise ValueError('Số chuỗi/index Android và WIPI không khớp; không tự đoán mapping.')
    lookup={r['index']:r for r in incoming};records=[]
    for r in original:
        new=dict(r);new['src']=lookup[r['index']]['text'];records.append(new)
    output_dump.parent.mkdir(parents=True,exist_ok=True)
    temporary=Path(tempfile.mkdtemp(prefix='.wipi-transfer-',dir=output_dump.parent))
    try:
        shutil.copytree(wipi_dump,temporary/'dump')
        write_direct_txt(temporary/'dump'/entry['txt'],[
            'WIPI text transferred from Android by index. Verify story alignment in game.',
            'path=script/output.dlg','type=TABLE32_REPOINT','encoding='+manifest['encoding'],
            'WIPI manifest and metadata retained; DLG remains RAW when imported.',
            'Display control differences warn by default; --controls strict rejects changes.'
        ],records)
        (temporary/'dump'/'_android_transfer_report.txt').write_text(
            'Copied %d records by index. Count/index validity is NOT a semantic alignment test.\n'%len(records),encoding='utf-8')
        os.replace(temporary/'dump',output_dump)
    finally: shutil.rmtree(temporary,ignore_errors=True)
    print('Đã chuyển %d chuỗi Android vào dump WIPI mới: %s'%(len(records),output_dump))


def default_output(source):
    p=Path(source)
    return p.with_name(p.stem+'_wipi_new.zip') if p.is_file() else p.with_name(p.name+'_wipi_new')


def menu():
    print('Zenonia WIPI/DSi AIO v1.3 - native/GUI fixed slots, binary dump RAW, DLG RAW, ZT1 zlib; không patch Android')
    ch=input('1. Xuất text + ảnh\n2. Nhập bản dịch\n3. Chuyển TXT Android sang dump WIPI\nChọn: ').strip()
    if ch=='3':
        android=input('File TXT Android: ').strip().strip('"')
        wipi=input('Thư mục dump WIPI có _manifest.json: ').strip().strip('"')
        out=input('Thư mục dump đầu ra mới: ').strip().strip('"')
        transfer_android_text(android,wipi,out);return 0
    source=Path(input('Thư mục dự án/nitrofs hoặc ZIP nguồn: ').strip().strip('"'))
    dump=Path(input('Thư mục dump [zenonia_wipi_text_dump]: ').strip().strip('"') or DUMP_DIR_NAME)
    if ch=='1':
        enc=input('Encoding cp949 / vi1 [cp949]: ').strip() or 'cp949'
        if enc not in ('cp949','vi1'): raise ValueError('Chọn cp949 hoặc vi1')
        export_game(source,dump,enc)
    elif ch=='2':
        out=input('Đầu ra mới [tự đặt tên]: ').strip().strip('"')
        target=input('Encoding chữ mới cp949 / vi1 [theo dump]: ').strip() or None
        import_game(source,dump,Path(out) if out else default_output(source),target_encoding=target)
    else: raise ValueError('Lựa chọn không hợp lệ')
    return 0


def main(argv=None):
    try:
        argv=sys.argv[1:] if argv is None else argv
        if not argv: return menu()
        parser=argparse.ArgumentParser(description='Zenonia WIPI/DSi text tool. DLG RAW offset32; ZT1 zlib. Native/GUI chỉ sửa slot đã xác minh; không repoint hay sửa font.')
        sub=parser.add_subparsers(dest='cmd',required=True)
        ex=sub.add_parser('export');ex.add_argument('source');ex.add_argument('dump',nargs='?',default=DUMP_DIR_NAME)
        ex.add_argument('--encoding',choices=['cp949','vi1'],default='cp949');ex.add_argument('--no-graphics',action='store_true')
        im=sub.add_parser('import');im.add_argument('source');im.add_argument('dump',nargs='?',default=DUMP_DIR_NAME);im.add_argument('output',nargs='?')
        im.add_argument('--unknown',choices=['strict','replace'],default='strict')
        im.add_argument('--target-encoding',choices=['cp949','vi1'],help='Mã hóa riêng chuỗi đã sửa; chuỗi chưa dịch giữ byte gốc.')
        im.add_argument('--controls',choices=['compatible','strict'],default='compatible',help='compatible: cho đổi !C/!N/màu và ghi cảnh báo; strict: phải giống nguồn.')
        tr=sub.add_parser('transfer');tr.add_argument('android_txt');tr.add_argument('wipi_dump');tr.add_argument('output_dump')
        a=parser.parse_args(argv)
        if a.cmd=='export': export_game(a.source,a.dump,a.encoding,not a.no_graphics)
        elif a.cmd=='import': import_game(a.source,a.dump,Path(a.output) if a.output else default_output(a.source),a.unknown,a.target_encoding,a.controls)
        else: transfer_android_text(a.android_txt,a.wipi_dump,a.output_dump)
        return 0
    except Exception as e:
        print('[LỖI]',e,file=sys.stderr)
        Path('zenonia_wipi_tool_error.log').write_text(traceback.format_exc(),encoding='utf-8')
        return 1


if __name__=='__main__': raise SystemExit(main())

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VioletDS AIO — single-file Text + Font editor.

This file combines the previous Text, Font, PNG and save backends in one
launchable script. Python >=3.10, tkinter, and Pillow are required.
It edits a VioletDS Round 3 extracted project; it does NOT repoint ELF strings
or patch font advance-width rendering. Run: python VioletDS_AIO.py
"""
from __future__ import annotations


# ==========================================================================
# PNG font pack parser
# ==========================================================================

"""Ragnarok Violet (WIPI) res/font container: extract or repack 8 indexed PNGs.

Usage:
  python font_pack_tool.py extract path/to/nitrofs/res/font output_dir
  python font_pack_tool.py pack output_dir path/to/new_font

This script does NOT rebuild the DS port's predecoded PNG cache. If glyph
pixels change, regenerate rv_png_indices.bin and rv_png_cache_table.h too.
"""


import argparse, hashlib, json, struct


from pathlib import Path


SIG=b'\x89PNG\r\n\x1a\n'


def get_png_info(blob: bytes):
    if not blob.startswith(SIG): raise ValueError('Not a PNG')
    off=8; ihdr=None; palette=False; ends=False
    while off<len(blob):
        if off+12>len(blob):raise ValueError('Incomplete PNG header')
        n=struct.unpack_from('>I',blob,off)[0]
        typ=blob[off+4:off+8]
        if off+12+n>len(blob):raise ValueError('Truncated PNG chunk')
        payload=blob[off+8:off+8+n]
        if typ==b'IHDR':
            if len(payload)!=13:raise ValueError('Incorrect IHDR')
            w,h,depth,ctype,compress,fil,interlace=struct.unpack('>IIBBBBB',payload)
            ihdr={'width':w,'height':h,'bit_depth':depth,'color_type':ctype,'interlace':interlace}
        if typ==b'PLTE':palette=True
        off+=12+n
        if typ==b'IEND':
            ends=True;break
    if not ihdr or not palette or ihdr['color_type']!=3 or not ends or off!=len(blob):
        raise ValueError('Expected complete indexed palette PNG (color type 3 / PLTE)')
    return ihdr


def sha(raw):return hashlib.sha256(raw).hexdigest()


def parse(data: bytes):
    if not data:raise ValueError('Empty font pack')
    count=data[0]; p=1; entries=[]
    if count!=8:raise ValueError(f'Expected 8 font strips, got {count}')
    for i in range(count):
        if p+2>len(data):raise ValueError('Truncated length')
        n=struct.unpack_from('<H',data,p)[0];p+=2
        if p+n>len(data):raise ValueError('Truncated PNG')
        raw=data[p:p+n];p+=n
        entries.append((raw,get_png_info(raw)))
    if p!=len(data):raise ValueError('Trailing bytes')
    return entries


def extract(src:Path,dst:Path):
    raw=src.read_bytes();ent=parse(raw);dst.mkdir(parents=True,exist_ok=True)
    index=[]
    for i,(png,meta) in enumerate(ent):
        (dst/f'{i}.png').write_bytes(png)
        index.append({'index':i,**meta,'png_bytes':len(png),'sha256':sha(png)})
    (dst/'manifest.json').write_text(json.dumps({'format':'Ragnarok Violet res/font','original_bytes':len(raw),'sha256':sha(raw),'images':index},indent=2)+'\n',encoding='utf-8')
    print(f'Extracted {len(ent)} indexed PNGs from {len(raw)} bytes into {dst}')


def pack(src:Path,dst:Path):
    original=json.loads((src/'manifest.json').read_text(encoding='utf-8'))
    if len(original['images'])!=8:raise ValueError('Invalid manifest')
    output=bytearray([8])
    for info in original['images']:
        i=info['index']; blob=(src/f'{i}.png').read_bytes();meta=get_png_info(blob)
        for key in ('width','height','bit_depth','color_type','interlace'):
            if meta[key]!=info[key]:
                raise ValueError(f'Font image {i}: {key} changed from {info[key]} to {meta[key]}')
        if len(blob)>65535:raise ValueError('PNG exceeds uint16 length')
        output+=struct.pack('<H',len(blob))+blob
    out=bytes(output);parse(out);dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(out)
    exact=sha(out)==original['sha256']
    print(f'Packed {len(out)} bytes -> {dst}; bit-exact original: {exact}')
    if not exact:print('WARNING: glyph PNG byte streams differ. Rebuild DS PNG cache before testing!')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=['extract','pack'])
    parser.add_argument('source',type=Path)
    parser.add_argument('destination',type=Path)
    a=parser.parse_args()
    (extract if a.mode=='extract' else pack)(a.source,a.destination)

# ==========================================================================
# Font atlas and glyph model
# ==========================================================================

"""Editable glyph regions for Ragnarok Violet res/font. Do not guess char maps.

Widths are region/pixel edit widths. Advance widths are stored in a JSON sidecar:
res/font contains no confirmed per-glyph metrics table. No runtime advance patch.
"""


from copy import deepcopy


from dataclasses import dataclass


from io import BytesIO


import zlib


from pathlib import Path


import json, os, struct, tempfile


from PIL import Image


def _replace_idat_keep_png_chunks(original:bytes,img:Image.Image)->bytes:
    """Preserve IHDR/PLTE/tRNS and original chunk order, change only IDAT.
    Ragnarok native PNG code reads embedded PLTE, so never expand it to 256.
    """
    meta=get_png_info(original)
    if meta['bit_depth']!=8 or meta['interlace']!=0:
        raise ValueError('Editing only supports original noninterlaced 8-bit indexed font PNGs')
    raw=img.tobytes()
    if any(v not in (0,1) for v in raw):
        raise ValueError('Font strip pixels must have palette index 0 or 1')
    w,h=img.size
    data=zlib.compress(b''.join(b'\0'+raw[y*w:(y+1)*w] for y in range(h)),9)
    def chunk(t,p):
        return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    out=bytearray(original[:8]);i=8;seen=False
    while i<len(original):
        length=struct.unpack_from('>I',original,i)[0]
        end=i+length+12
        typ=original[i+4:i+8]
        if typ==b'IDAT':
            if not seen:out+=chunk(b'IDAT',data);seen=True
        else:out+=original[i:end]
        i=end
    if not seen:raise ValueError('Original PNG has no IDAT')
    return bytes(out)


def pack_images(images, original_pngs, baseline):
    output = bytearray([8])
    for i, img in enumerate(images):
        if img.mode != 'P' or img.size != baseline[i].size:
            raise ValueError(f'Atlas {i}: mode or dimensions changed')
        if img.tobytes() == baseline[i].tobytes():
            raw = original_pngs[i]
        else:
            # Keep original palette, chunk order and byte depth. Only IDAT changes.
            raw = _replace_idat_keep_png_chunks(original_pngs[i],img)
        meta = get_png_info(raw)
        expected = get_png_info(original_pngs[i])
        for k in ('width','height','bit_depth','color_type','interlace'):
            if meta[k] != expected[k]:
                raise ValueError(f'Atlas {i}: {k} changed: {meta[k]} vs {expected[k]}')
        if len(raw)>65535:raise ValueError(f'Atlas {i} exceeds 65535 bytes')
        output.extend(struct.pack('<H', len(raw)))
        output.extend(raw)
    packed = bytes(output)
    parse(packed)
    return packed


def suggest_regions(img):
    """Candidate raster runs only. Blank rows are NOT verified glyph cuts."""
    w,h=img.size
    occupied = [any(img.getpixel((x,y)) != 0 for x in range(w)) for y in range(h)]
    runs=[];start=None
    for y,present in enumerate(occupied+[False]):
        if present and start is None:start=y
        if not present and start is not None:
            if y-start>=2:
                runs.append({'name':f'Candidate {len(runs)+1}', 'y':start,
                             'height':y-start,'width':w,'advance':w,'provisional':True})
            start=None
    return runs


class FontDocument:
    def __init__(self,font_path:Path, region_path:Path|None=None):
        self.font_path=Path(font_path)
        self.region_path=Path(region_path) if region_path else self.font_path.parent.parent/'translation'/'font_glyph_regions.json'
        self.original_data=self.font_path.read_bytes()
        entries=parse(self.original_data)
        self.original_pngs=[ent[0] for ent in entries]
        self.images=[]
        for png in self.original_pngs:
            with Image.open(BytesIO(png)) as im:
                im.load(); self.images.append(im.copy())
        self.baseline=[im.copy() for im in self.images]
        self.regions=[[{'name':'Manual region','y':0,'height':min(10,im.height),
                        'width':im.width,'advance':im.width,'provisional':True}]
                        for im in self.images]
        if self.region_path.exists():
            self._load_regions()
        self.modified=False
        self.history=[]

    def _load_regions(self):
        data=json.loads(self.region_path.read_text(encoding='utf-8'))
        if data.get('format')!='violet-font-regions-v1':
            raise ValueError('Unsupported glyph regions metadata')
        sections=data['atlases']
        if len(sections)!=8:raise ValueError('Metadata must contain 8 atlases')
        for idx,regions in enumerate(sections):
            new=[]
            for reg in regions:
                reg={k:reg[k] for k in ('name','y','height','width','advance')}
                self._validate(idx,reg)
                new.append(reg)
            self._validate_disjoint(new)
            self.regions[idx]=new

    def _validate(self,index,reg):
        w,h=self.images[index].size
        if not (isinstance(reg['y'],int) and isinstance(reg['height'],int) and
                isinstance(reg['width'],int) and isinstance(reg['advance'],int)):
            raise ValueError('Glyph metrics must be integers')
        if not (0<=reg['y']<h and 1<=reg['height']<=h-reg['y']):
            raise ValueError(f'Region outside atlas {index}')
        if not (1<=reg['width']<=w):raise ValueError(f'Width must be 1..{w}')
        if not (1<=reg['advance']<=32):raise ValueError('Advance draft must be 1..32')
        if not isinstance(reg['name'],str):raise ValueError('Region name must be text')

    @staticmethod
    def _validate_disjoint(regions):
        sorted_regs=sorted(regions,key=lambda r:r['y'])
        for a,b in zip(sorted_regs,sorted_regs[1:]):
            if a['y']+a['height']>b['y']:
                raise ValueError(f'Overlapping regions: {a["name"]} and {b["name"]}')

    def add_region(self,index,y,height,width=None,name=None):
        w,_=self.images[index].size
        reg={'name':name or f'Glyph {len(self.regions[index])+1}', 'y':int(y),
             'height':int(height),'width':w if width is None else int(width),
             'advance':w,'provisional':True}
        self._validate(index,reg)
        self._validate_disjoint(self.regions[index]+[reg])
        self.snapshot()
        self.regions[index].append(reg)
        self.regions[index].sort(key=lambda r:r['y'])
        self.modified=True
        return self.regions[index].index(reg)

    def delete_region(self,index,region_index):
        self.snapshot()
        self.regions[index].pop(region_index)
        self.modified=True

    def update_region(self,index,region_index, **fields):
        reg=deepcopy(self.regions[index][region_index]);reg.update(fields)
        self._validate(index,reg)
        tmp=deepcopy(self.regions[index]);tmp[region_index]=reg
        self._validate_disjoint(tmp)
        self.snapshot()
        # Shrinking the bitmap width removes the clipped pixels as requested.
        if reg['width']<self.regions[index][region_index]['width']:
            for y in range(reg['y'],reg['y']+reg['height']):
                for x in range(reg['width'],self.images[index].width):
                    self.images[index].putpixel((x,y),0)
        self.regions[index]=tmp
        self.modified=True

    def detect_candidates(self,index):
        proposed=suggest_regions(self.images[index])
        self._validate_disjoint(proposed)
        self.snapshot();self.regions[index]=proposed;self.modified=True
        return len(proposed)

    def snapshot(self):
        self.history.append(([im.copy() for im in self.images],deepcopy(self.regions)))
        if len(self.history)>25:self.history.pop(0)

    def undo(self):
        if not self.history:return False
        self.images,self.regions=self.history.pop()
        self.modified=True
        return True

    def stroke(self,index,reg_index,x,y,pen):
        reg=self.regions[index][reg_index]
        if not (0<=x<reg['width'] and 0<=y<reg['height']):return False
        ry=reg['y']+y
        if self.images[index].getpixel((x,ry))==pen:return False
        self.images[index].putpixel((x,ry),pen);self.modified=True
        return True

    def clear_region(self,index,reg_index):
        self.snapshot();r=self.regions[index][reg_index]
        for y in range(r['y'],r['y']+r['height']):
            for x in range(self.images[index].width):self.images[index].putpixel((x,y),0)
        self.modified=True

    def copy_glyph(self,index,region_index):
        r=self.regions[index][region_index]
        pixels=[[self.images[index].getpixel((x,y)) for x in range(r['width'])]
                for y in range(r['y'],r['y']+r['height'])]
        return {'format':'violet-glyph-v1','name':r['name'], 'width':r['width'],
                'height':r['height'],'advance':r['advance'],'pixels':pixels}

    def paste_glyph(self,index,region_index,data):
        if data.get('format')!='violet-glyph-v1':raise ValueError('Not a Violet glyph clipboard')
        current=self.regions[index][region_index]
        reg={'name':str(data['name']),'y':current['y'],'width':int(data['width']),
             'height':int(data['height']),'advance':int(data['advance'])}
        self._validate(index,reg)
        self._validate_disjoint([r for i,r in enumerate(self.regions[index]) if i!=region_index]+[reg])
        if len(data['pixels'])!=reg['height'] or any(len(row)!=reg['width'] or
                any(v not in (0,1) for v in row) for row in data['pixels']):
            raise ValueError('Invalid copied glyph dimensions/palette')
        self.snapshot()
        # Clear old AND newly occupied span; validation proved neither touches
        # another region. Eliminates ghost pixels outside pasted glyph width.
        y0=min(current['y'],reg['y'])
        y1=max(current['y']+current['height'],reg['y']+reg['height'])
        for y in range(y0,y1):
            for x in range(self.images[index].width):self.images[index].putpixel((x,y),0)
        for y,row in enumerate(data['pixels']):
            for x,v in enumerate(row):self.images[index].putpixel((x,reg['y']+y),v)
        self.regions[index][region_index]=reg
        self.modified=True

    def packed(self):
        return pack_images(self.images,self.original_pngs,self.baseline)

    def regions_bytes(self):
        data={'format':'violet-font-regions-v1','warning':'Manual regions/advance draft; game glyph map NOT verified; advance not patched.',
              'atlases':self.regions}
        return (json.dumps(data,indent=2,ensure_ascii=False)+'\n').encode('utf-8')

# ==========================================================================
# Original-project PNG cache builder
# ==========================================================================

"""Build project PNG cache in temporary files using the game's packer code.
Callers commit res/font, rv_png_indices.bin and rv_png_cache_table.h together.
"""


import importlib.util,sys,zlib


from pathlib import Path


def rebuild(root:Path, override_font:bytes, cache_out:Path, header_out:Path):
    root=Path(root).resolve()
    tool=root/'tools'/'pack_ragnarok_assets.py'
    if not tool.exists():raise FileNotFoundError(f'Missing project packer: {tool}')
    name='_violet_original_png_cache_builder'
    spec=importlib.util.spec_from_file_location(name,tool)
    mod=importlib.util.module_from_spec(spec)
    sys.modules[name]=mod
    try:spec.loader.exec_module(mod)
    finally:sys.modules.pop(name,None)
    nitro=root/'nitrofs'
    if not nitro.exists():raise FileNotFoundError(f'No nitrofs directory in {root}')
    cache_by_key={};count=0
    for path in sorted(nitro.rglob('*')):
        if not path.is_file() or '_port' in path.relative_to(nitro).parts:continue
        blob=override_font if path==nitro/'res'/'font' else path.read_bytes()
        for offset,png in mod.extract_pngs(blob):
            w,h,depth,interlace,idat=mod.png_meta(png)
            key=mod.Key(zlib.crc32(idat)&0xffffffff,mod.fnv1a(idat),len(idat),w,h,depth,interlace)
            pixels=mod.decode_indices(png,w,h)
            old=cache_by_key.get(key)
            if old is not None and old!=pixels:
                raise RuntimeError(f'PNG cache hash collision at {path}:{offset}')
            cache_by_key.setdefault(key,pixels)
            count+=1
    ordered=sorted(cache_by_key,key=lambda k:(k.crc32,k.fnv1a,k.width,k.height,k.bit_depth,k.interlace,k.idat_len))
    cache_out.parent.mkdir(parents=True,exist_ok=True)
    rows=[];pos=0
    with cache_out.open('wb') as out:
        for k in ordered:
            pixels=cache_by_key[k];out.write(pixels)
            rows.append(mod.CacheRecord(k,pos,len(pixels)));pos+=len(pixels)
    mod.write_header(header_out,rows)
    return {'seen_pngs':count,'unique_pngs':len(rows),'cache_bytes':pos}

# ==========================================================================
# Transactional writer
# ==========================================================================

"""Transactional project write: staged PNG cache and automatic backups."""


from pathlib import Path


import os, shutil, tempfile


def commit_font(document,root):
    root=Path(root).resolve()
    desired={
      root/'nitrofs'/'res'/'font':document.packed(),
      document.region_path:document.regions_bytes(),
    }
    with tempfile.TemporaryDirectory(prefix='violet-font-stage-') as td:
        td=Path(td)
        summary=rebuild(root,desired[root/'nitrofs'/'res'/'font'], td/'cache.bin', td/'table.h')
        desired[root/'nitrofs'/'_port'/'rv_png_indices.bin']=(td/'cache.bin').read_bytes()
        desired[root/'source'/'rv_png_cache_table.h']=(td/'table.h').read_bytes()
        # Only changed files need commit. Prevents surprising cache rewrites on no-op.
        replacements={p:data for p,data in desired.items() if not p.exists() or p.read_bytes()!=data}
        if not replacements:return {**summary,'written':0,'backup_paths':[]}
        old={p:(p.read_bytes() if p.exists() else None) for p in replacements}
        backups=[];done=[]
        try:
            for path,data in replacements.items():
                path.parent.mkdir(parents=True,exist_ok=True)
                if old[path] is not None:
                    bak=root/'.violet_aio_backups'/path.relative_to(root)
                    if not bak.exists():
                        bak.parent.mkdir(parents=True,exist_ok=True)
                        bak.write_bytes(old[path]);backups.append(str(bak))
                stage=path.with_name(path.name+'.violettmp')
                stage.write_bytes(data)
                os.replace(stage,path)
                done.append(path)
        except Exception:
            for p in reversed(done):
                if old[p] is None:
                    p.unlink(missing_ok=True)
                else:
                    stage=p.with_name(p.name+'.violetrollback')
                    stage.write_bytes(old[p]);os.replace(stage,p)
            raise
    document.original_data=desired[root/'nitrofs'/'res'/'font']
    document.original_pngs=[entry[0] for entry in parse(document.original_data)]
    document.baseline=[im.copy() for im in document.images]
    document.modified=False
    document.history.clear()
    return {**summary,'written':len(replacements),'backup_paths':backups}

# ==========================================================================
# Text export, import and validation
# ==========================================================================

"""Ragnarok Violet WIPI text export/import. Python standard library only."""


import argparse, collections, csv, hashlib, json, os, re, shutil, struct, sys, tempfile, unicodedata, zipfile


from pathlib import Path, PurePosixPath


VERSION=1


TOOL='ragnarok-violet-text'


VI_ORDER='áàảãạâấầẩẫậăắằẳẵặđéèẻẽẹêếềểễệíìỉĩịóòỏõọôốồổỗộơớờởỡợúùủũụưứừửữựýỳỷỹỵ'


VI_MAP={ch:0x80+i for i,ch in enumerate(VI_ORDER)}


PROFILE_SHAS={'4aff4641efeff7578afc6d02c44b5c45279648200bb5af6e9db1996778ec609a', '76887add62f3bf54d61b35fd8c9e1b3f2b39fad3980d770a5ca0fd321f24396f'}


ROUND3_CODE_SHA='6b07869d105ee2601174c1a498a30ad6e1d5932c4dd516703eb2edfe6b9ea657'


ROUND3_TEXT_SIZE=0x3e04f


ROUND3_DATA_SIZE=0x15ab4


PNG=b'\x89PNG\r\n\x1a\n'


PRINTF=re.compile(r'%(?:\d+\$)?[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn%]')


class TextError(ValueError): pass


def sha(b): return hashlib.sha256(b).hexdigest()


def safe_path(name):
    p=PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name or '\x00' in name:
        raise TextError('Unsafe path: '+repr(name))
    return p


def read_json(p): return json.loads(Path(p).read_text(encoding='utf-8-sig'))


def write_json(p,obj): Path(p).write_text(json.dumps(obj,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


class Game:
    def __init__(self,path):
        self.path=Path(path).resolve(); self.z=None
        if self.path.is_dir():
            self.root=self.path/'nitrofs' if (self.path/'nitrofs/binary.mod').is_file() else self.path
            if not (self.root/'binary.mod').is_file(): raise TextError('Choose project/nitrofs containing binary.mod')
            files=[p for p in self.root.rglob('*') if p.is_file()]
            if any(p.is_symlink() for p in self.root.rglob('*')): raise TextError('Symlinks are unsupported')
            self.names=sorted(p.relative_to(self.root).as_posix() for p in files)
        else:
            self.z=zipfile.ZipFile(self.path)
            infos=self.z.infolist(); names=[i.filename for i in infos]
            if len(names)!=len(set(names)): raise TextError('Duplicate ZIP paths')
            for i in infos:
                safe_path(i.filename)
                if (i.external_attr>>16)&0o170000==0o120000: raise TextError('ZIP symlink')
            roots=[n[:-10] for n in names if n=='binary.mod' or n.endswith('/binary.mod')]
            if len(roots)!=1: raise TextError('ZIP/JAR must contain one binary.mod')
            self.prefix=roots[0]; self.names=sorted(n[len(self.prefix):] for n in names if n.startswith(self.prefix) and not n.endswith('/'))
        for n in self.names: safe_path(n)
        if not any(n.startswith('res/diaData/') for n in self.names): raise TextError('Not a Ragnarok Violet resource tree')
    def read(self,n):
        safe_path(n)
        return self.z.read(self.prefix+n) if self.z else (self.root/n).read_bytes()
    def close(self):
        if self.z:self.z.close()


def decode(b):
    t=b.decode('cp949',errors='surrogateescape'); out=[]
    for ch in t:
        n=ord(ch)
        if ch=='\\':out.append('\\\\')
        elif 0xdc80<=n<=0xdcff:out.append('\\x%02X'%(n-0xdc00))
        elif n<32 and ch not in '\r\n\t':out.append('\\x%02X'%n)
        elif n==127:out.append('\\x7F')
        else:out.append(ch)
    return ''.join(out)


def encode(t,encoding='cp949'):
    t=unicodedata.normalize('NFC',t);out=bytearray();i=0
    while i<len(t):
        if t[i]=='\\':
            if t[i:i+2]=='\\\\':out.append(92);i+=2;continue
            if re.fullmatch(r'\\x[0-9a-fA-F]{2}',t[i:i+4]):out.append(int(t[i+2:i+4],16));i+=4;continue
            raise TextError('Use \\\\ for literal backslash, or \\xHH for a raw byte')
        ch=t[i]
        try:
            if encoding=='vi1':
                if ch in VI_MAP:out.append(VI_MAP[ch])
                elif ord(ch)<128:out.append(ord(ch))
                else:raise UnicodeError('outside VI1 mapping')
            else:out.extend(ch.encode(encoding))
        except UnicodeError as e:raise TextError(f'{ch!r} cannot be encoded as {encoding}; no replacement was written') from e
        i+=1
    return bytes(out)


def parse_u8(b):
    p=0;rows=[]
    while p<len(b):
        n=b[p];p+=1
        if p+n>len(b):raise TextError('Truncated length8 table')
        rows.append((p,b[p:p+n],None));p+=n
    return rows


def parse_dialogue(b):
    if not b:raise TextError('Empty dialogue')
    p=1;rows=[]
    for i in range(b[0]):
        if p+3>len(b):raise TextError('Truncated dialogue record')
        portrait=b[p];n=struct.unpack_from('<H',b,p+1)[0];p+=3
        if p+n>len(b):raise TextError('Truncated dialogue text')
        rows.append((p,b[p:p+n],portrait));p+=n
    if p!=len(b):raise TextError('Trailing bytes in dialogue')
    return rows


def elf_sections(b):
    if b[:6]!=b'\x7fELF\x01\x01':raise TextError('Expected ELF32 LE')
    off=struct.unpack_from('<I',b,32)[0];size,count,idx=struct.unpack_from('<HHH',b,46)
    if size<40 or idx>=count or off+size*count>len(b):raise TextError('Bad ELF sections')
    ss=[struct.unpack_from('<10I',b,off+i*size) for i in range(count)]
    for s in ss:
        if s[1]!=8 and s[4]+s[5]>len(b):raise TextError('ELF section exceeds file')
    return ss


def native_records(b):
    sections=elf_sections(b);refs=set()
    for s in sections:
        if s[1]!=9:continue
        for p in range(s[4],s[4]+s[5],s[9] or 8):
            site,info=struct.unpack_from('<II',b,p)
            if info&255!=2:continue
            for target in sections:
                if target[1]==1 and target[3]<=site<target[3]+target[5]-3:
                    refs.add(struct.unpack_from('<I',b,target[4]+site-target[3])[0])
                    break
    # Strict EUC-KR Hangul bytes plus ASCII text. Non-Hangul runs require a
    # relocated pointer; unreferenced spans require per-row human review.
    rx=re.compile(rb'(?:[\x20-\x7e\r\n\t]|[\xb0-\xc8][\xa1-\xfe])+\x00')
    rows=[];profile=sha(b) in PROFILE_SHAS
    for sec in sections:
        if (sec[3]==0x1000 and sec[5]==ROUND3_TEXT_SIZE and
            any(d[3]==0x1400000 and d[5]==ROUND3_DATA_SIZE for d in sections)):
            stable_end=0x3b0b0-0x1000
            if sha(b[sec[4]:sec[4]+stable_end])==ROUND3_CODE_SHA:
                profile=True
    for s in sections:
        if s[1]!=1 or not s[2]&2:continue
        begin=s[4]
        if s[3]==0x1000:begin+=max(0,0x3b0b0-s[3])
        elif s[3]!=0x1400000:continue
        for m in rx.finditer(b,begin,s[4]+s[5]):
            raw=m.group()[:-1];off=m.start();va=s[3]+off-s[4]
            if not raw or len(raw)>4096:continue
            try:t=raw.decode('cp949')
            except UnicodeError:continue
            hangul=any('\uac00'<=ch<='\ud7a3' for ch in t)
            if not hangul and (va not in refs or not re.search('[A-Za-z]{2}',t)):continue
            if not hangul and (re.search(r'(?:res/|\.(?:png|dat|smd|std|mmf)|save%d|^[a-z0-9_./]+$)',t) or t.startswith(('MC_','WIPI'))):continue
            rows.append((off,raw,{'va':va,'referenced':va in refs,'profile_verified':profile}))
    return rows


def record(path,kind,index,off,raw,meta):
    source=decode(raw)
    return dict(id=f'{path}:{kind}:{index:05d}',source_file=path,kind=kind,index=index,
                offset=off,source=source,source_hash=sha(source.encode('utf-8')),target='',
                status='NEW',reviewed=False,max_bytes=255 if kind=='length8' else 65535 if kind=='dialogue' else len(raw),
                metadata=meta,context=path,static_status='NOT_RUN',layout_status='NOT_RUN',runtime_status='NOT_RUN')


def collect(game):
    records=[];inventory=[];images=[]
    for name in game.names:
        b=game.read(name);kind=None;items=[]
        if name.startswith('res/diaData/'):
            kind='dialogue';items=parse_dialogue(b)
        elif name in {f'res/{i}.dat' for i in range(5)}:
            kind='length8';items=parse_u8(b)
        elif name=='binary.mod':kind='native';items=native_records(b)
        for i,(off,raw,meta) in enumerate(items):records.append(record(name,kind,i,off,raw,meta))
        # Index every PNG, including embedded PNG containers. These are assets,
        # not editable text records. Do not pretend raster labels are strings.
        pos=0
        while (a:=b.find(PNG,pos))>=0:
            end=a+8
            while True:
                if end+12>len(b):raise TextError('Truncated PNG: '+name)
                n=struct.unpack_from('>I',b,end)[0];typ=b[end+4:end+8];end+=12+n
                if end>len(b):raise TextError('PNG exceeds file: '+name)
                if typ==b'IEND':break
            images.append(dict(source_file=name,offset=a,length=end-a,width=struct.unpack_from('>I',b,a+16)[0],height=struct.unpack_from('>I',b,a+20)[0],status='NEEDS_VISUAL_REVIEW'))
            pos=end
        category='text' if kind else 'png_or_container' if PNG in b else 'binary_not_text'
        inventory.append(dict(path=name,sha256=sha(b),size=len(b),category=category,format=kind,records=len(items)))
    return records,inventory,images


def export_game(source,dump,graphics=True):
    dump=Path(dump).resolve()
    if dump.exists():raise TextError('Dump exists; choose a new folder')
    game=Game(source)
    try:
        # Never put exports inside the source tree.
        if game.path.is_dir() and dump.is_relative_to(game.path):raise TextError('Dump must be outside source')
        rows,files,images=collect(game)
        dump.parent.mkdir(parents=True,exist_ok=True)
        temp=Path(tempfile.mkdtemp(prefix='.rv-export-',dir=dump.parent))
        try:
            with (temp/'text.jsonl').open('w',encoding='utf-8',newline='\n') as f:
                for row in rows:f.write(json.dumps(row,ensure_ascii=False)+'\n')
            with (temp/'text.csv').open('w',encoding='utf-8-sig',newline='') as f:
                w=csv.DictWriter(f,fieldnames=['id','source','target','reviewed','max_bytes','source_hash','source_file','kind']);w.writeheader()
                for row in rows:w.writerow({k:row[k] for k in w.fieldnames})
            write_json(temp/'manifest.json',dict(tool=TOOL,version=VERSION,source_encoding='cp949',binary_sha256=sha(game.read('binary.mod')),files=files,record_count=len(rows)))
            write_json(temp/'coverage_inventory.json',dict(records=len(rows),by_kind=dict(collections.Counter(r['kind'] for r in rows)),files=files,images=images,runtime_status='NOT_RUN',font_status='NOT_RUN',notes=['All resources inventoried; raster text requires image editing.','Native spans without a relocated string pointer require reviewed=true.','Native strings retain original byte capacity; no ELF repointing.']))
            if graphics:
                for i,im in enumerate(images):
                    p=temp/'images'/f'{i:04d}.png';p.parent.mkdir(exist_ok=True)
                    p.write_bytes(game.read(im['source_file'])[im['offset']:im['offset']+im['length']])
            os.replace(temp,dump)
        finally:
            if temp.exists():shutil.rmtree(temp)
        print(json.dumps(dict(dump=str(dump),records=len(rows),by_kind=dict(collections.Counter(r['kind'] for r in rows)),images=len(images)),ensure_ascii=True))
        return rows
    finally:game.close()


def load_edits(dump,table=None):
    path=Path(table) if table else Path(dump)/'text.jsonl'
    if path.suffix.lower()=='.csv':
        with path.open(encoding='utf-8-sig',newline='') as f:return list(csv.DictReader(f))
    return [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]


def syntax(text):
    return ([m.group() for m in PRINTF.finditer(text)],re.findall(r'\\x[0-9A-Fa-f]{2}',text))


def prepare(game,dump,encoding='cp949',table=None):
    manifest=read_json(Path(dump)/'manifest.json')
    if manifest.get('tool')!=TOOL or manifest.get('version')!=VERSION:raise TextError('Wrong dump format; export Ragnarok again')
    rows,files,images=collect(game)
    # Image/font edits do not invalidate a text dump; compare only text-bearing
    # files while preserving the exact ELF signature and every text record.
    exported_text={r['path']:(r['sha256'],r['records']) for r in manifest['files'] if r['category']=='text'}
    current_text={r['path']:(r['sha256'],r['records']) for r in files if r['category']=='text'}
    if exported_text!=current_text or manifest.get('binary_sha256')!=sha(game.read('binary.mod')):
        raise TextError('Text-bearing source files changed since export')
    edits=load_edits(dump,table);by_id={}
    for row in edits:
        if row['id'] in by_id:raise TextError('Duplicate ID: '+row['id'])
        by_id[row['id']]=row
    if set(by_id)!={r['id'] for r in rows}:raise TextError('Missing/extra IDs')
    replacements={};changes=[];encoded={}
    for row in rows:
        edit=by_id[row['id']]
        if edit.get('source')!=row['source'] or edit.get('source_hash')!=row['source_hash']:raise TextError('Source text/hash changed: '+row['id'])
        target=edit.get('target','')
        if not isinstance(target,str):raise TextError('target must be text')
        if target=='' or target==row['source']:continue
        if row['kind']=='native':
            if not row['metadata']['profile_verified']:raise TextError('Unrecognized ELF revision; native writes disabled')
            if not row['metadata']['referenced'] and str(edit.get('reviewed',False)).lower() not in ('true','1','yes'):raise TextError('Review native candidate first: '+row['id'])
        if syntax(target)!=syntax(row['source']):raise TextError('printf/raw control bytes changed: '+row['id'])
        raw=encode(target,encoding)
        if b'\0' in raw and '\\x00' not in row['source']:raise TextError('Embedded NUL: '+row['id'])
        if len(raw)>row['max_bytes']:raise TextError(f"{row['id']}: {len(raw)} bytes > {row['max_bytes']}")
        encoded[row['id']]=raw;changes.append(dict(id=row['id'],bytes=len(raw)))
    grouped=collections.defaultdict(list)
    for row in rows:grouped[row['source_file']].append(row)
    for name,rs in grouped.items():
        if not any(r['id'] in encoded for r in rs):continue
        original=game.read(name);kind=rs[0]['kind']
        if kind=='native':
            out=bytearray(original)
            for row in rs:
                if row['id'] in encoded:
                    raw=encoded[row['id']];a=row['offset'];n=row['max_bytes'];out[a:a+n]=raw+b'\0'*(n-len(raw))
            if len(out)!=len(original):raise TextError('ELF size changed')
        else:
            out=bytearray([len(rs)]) if kind=='dialogue' else bytearray()
            for row in rs:
                raw=encoded.get(row['id'])
                if raw is None:
                    n=(struct.unpack_from('<H',original,row['offset']-2)[0] if kind=='dialogue' else original[row['offset']-1])
                    raw=original[row['offset']:row['offset']+n]
                if kind=='dialogue':out+=bytes([row['metadata']])+struct.pack('<H',len(raw))+raw
                else:out+=bytes([len(raw)])+raw
            (parse_dialogue if kind=='dialogue' else parse_u8)(out)
        replacements[name]=bytes(out)
    return replacements,dict(changed_records=len(changes),changed_files=len(replacements),changes=changes,target_encoding=encoding,static_status='PASS',layout_status='NOT_RUN',runtime_status='NOT_RUN',font_status='NOT_RUN')


def import_game(source,dump,output,encoding='cp949',table=None,check=False):
    output=Path(output).resolve() if output else None;dump=Path(dump).resolve();game=Game(source)
    try:
        replacements,report=prepare(game,dump,encoding,table)
        if check:print(json.dumps(report,ensure_ascii=True));return report
        if not output or output.exists():raise TextError('Choose a new output path')
        if game.path.is_dir() and (output.is_relative_to(game.path) or game.path.is_relative_to(output)):raise TextError('Output overlaps source')
        if output==dump or output.is_relative_to(dump) or dump.is_relative_to(output):raise TextError('Output overlaps dump')
        output.parent.mkdir(parents=True,exist_ok=True)
        temp=Path(tempfile.mkdtemp(prefix='.rv-import-',dir=output.parent))
        try:
            if game.z:
                if output.suffix.lower() not in ('.zip','.jar'):raise TextError('Archive output needs .zip or .jar')
                with zipfile.ZipFile(temp/'result','w') as z:
                    for info in game.z.infolist():
                        n=info.filename[len(game.prefix):] if info.filename.startswith(game.prefix) else None
                        z.writestr(info,replacements.get(n,game.z.read(info.filename)))
                os.replace(temp/'result',output)
            else:
                shutil.copytree(game.path,temp/'result')
                root=temp/'result'/'nitrofs' if game.root!=game.path else temp/'result'
                for name,b in replacements.items():(root/name).write_bytes(b)
                os.replace(temp/'result',output)
            write_json(output.with_name(output.name+'.translation-report.json'),report)
        finally:shutil.rmtree(temp,ignore_errors=True)
        print(json.dumps(dict(output=str(output),**report),ensure_ascii=True));return report
    finally:game.close()


def main(argv=None):
    try:
        argv=sys.argv[1:] if argv is None else argv
        if not argv:
            print('Ragnarok Violet text tool - export/import/check (CP949, length8, dialogue, native ELF)')
            action=input('1 Export / 2 Import / 3 Check: ').strip();source=input('Project, nitrofs, ZIP or JAR: ').strip().strip('"');dump=input('Dump folder: ').strip().strip('"')
            if action=='1':export_game(source,dump);return 0
            encoding=input('Target cp949 / ascii / vi1 [ascii]: ').strip() or 'ascii'
            output=None if action=='3' else input('New output path: ').strip().strip('"')
            import_game(source,dump,output,encoding,check=action=='3');return 0
        p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
        ex=sub.add_parser('export');ex.add_argument('source');ex.add_argument('dump');ex.add_argument('--no-graphics',action='store_true')
        for cmd in ['import','check']:
            q=sub.add_parser(cmd);q.add_argument('source');q.add_argument('dump')
            if cmd=='import':q.add_argument('output')
            q.add_argument('--target-encoding',choices=['cp949','ascii','vi1'],default='cp949');q.add_argument('--table',help='Optional edited text.csv instead of text.jsonl')
        a=p.parse_args(argv)
        if a.command=='export':export_game(a.source,a.dump,not a.no_graphics)
        else:
            if a.target_encoding=='vi1':print('VI1 encodes bytes only; game renderer/font does not yet support Vietnamese accents.',file=sys.stderr)
            import_game(a.source,a.dump,getattr(a,'output',None),a.target_encoding,a.table,a.command=='check')
        return 0
    except (TextError,ValueError,OSError,zipfile.BadZipFile) as e:
        print('ERROR: '+str(e),file=sys.stderr);return 1

# ==========================================================================
# Two-tab Tk editor
# ==========================================================================

"""Ragnarok Violet WIPI / VioletDS: two-tab Text + Font editor."""


import json, queue, threading, time, traceback


from pathlib import Path


import tkinter as tk


from tkinter import filedialog, messagebox, simpledialog, ttk


CLIP_PREFIX='VIOLET_GLYPH_V1:'


class App:
    def __init__(self,root):
        self.root=root;self.busy=False;self.doc=None;self.current_atlas=0
        self.current_reg=0;self.cells={};self.messages=queue.Queue();self.drawing=False
        root.title('Ragnarok Violet — Text + Font AIO');root.geometry('1180x790');root.minsize(950,670)
        self.style=ttk.Style()
        if 'clam' in self.style.theme_names():self.style.theme_use('clam')
        self.project=tk.StringVar();self.dump=tk.StringVar()
        self.atlas_var=tk.StringVar(value='0');self.name=tk.StringVar();self.ry=tk.StringVar(value='0')
        self.rh=tk.StringVar(value='10');self.rw=tk.StringVar(value='5');self.adv=tk.StringVar(value='5')
        self.eraser=tk.BooleanVar(value=False)
        self._make_ui()
        root.after(100,self.process_messages)
        root.bind('<Control-c>',lambda e:self.copy_glyph() if self.nb.index('current')==1 else None)
        root.bind('<Control-v>',lambda e:self.paste_glyph() if self.nb.index('current')==1 else None)

    def _make_ui(self):
        top=ttk.Frame(self.root,padding=10);top.pack(fill='x')
        ttk.Label(top,text='Project / ZIP:').pack(side='left',padx=(0,8))
        ttk.Entry(top,textvariable=self.project).pack(side='left',fill='x',expand=True,padx=4)
        ttk.Button(top,text='Folder...',command=self.choose_project).pack(side='left',padx=3)
        ttk.Button(top,text='ZIP...',command=self.choose_zip).pack(side='left')
        self.nb=ttk.Notebook(self.root);self.nb.pack(fill='both',expand=True,padx=10,pady=(0,8))
        self.text_page=ttk.Frame(self.nb,padding=14);self.font_page=ttk.Frame(self.nb,padding=9)
        self.nb.add(self.text_page,text='Text');self.nb.add(self.font_page,text='Font')
        self._make_text_tab();self._make_font_tab()
        self.status=tk.StringVar(value='Ready. Font advance values are NOT patched into game runtime.')
        ttk.Label(self.root,textvariable=self.status,anchor='w',padding=(10,5)).pack(fill='x')

    def choose_project(self):
        p=filedialog.askdirectory(title='Select extracted VioletDS project (contains nitrofs)')
        if p:self.project.set(p);self.doc=None
    def choose_zip(self):
        p=filedialog.askopenfilename(title='Select project ZIP',filetypes=[('ZIP / JAR','*.zip *.jar')])
        if p:self.project.set(p);self.doc=None

    def _make_text_tab(self):
        text=self.text_page
        ttk.Label(text,text='Export / Import / Check text',font=('Arial',15,'bold')).pack(anchor='w')
        ttk.Label(text,text='Round 3 text backend: binary.mod, res/0–4.dat, res/diaData. Output CSV is editable in Excel; retain IDs, hashes and placeholders.').pack(anchor='w',pady=(7,12))
        bar=ttk.Frame(text);bar.pack(anchor='w')
        for label,fn in [('EXPORT',self.text_export),('IMPORT',self.text_import),('CHECK',self.text_check)]:
            ttk.Button(bar,text=label,command=fn,width=17).pack(side='left',padx=(0,10))
        line=ttk.Frame(text);line.pack(fill='x',pady=(18,8))
        ttk.Label(line,text='Dump folder:').pack(side='left')
        ttk.Entry(line,textvariable=self.dump).pack(side='left',fill='x',expand=True,padx=(10,0))
        ttk.Label(text,text='Export tạo text.csv + text.jsonl. Import/Check ưu tiên text.csv nếu tồn tại. Import luôn tạo project/ZIP mới, không ghi đè dữ liệu gốc.').pack(anchor='w',pady=(0,12))
        frame=ttk.LabelFrame(text,text='Report / log',padding=6);frame.pack(fill='both',expand=True)
        self.log_box=tk.Text(frame,wrap='word',font=('Consolas',10),height=16,state='disabled',background='#101820',foreground='#cde5e9')
        self.log_box.pack(side='left',fill='both',expand=True)
        sb=ttk.Scrollbar(frame,orient='vertical',command=self.log_box.yview);sb.pack(side='right',fill='y')
        self.log_box.configure(yscrollcommand=sb.set)
        self.log('Choose a project (folder or ZIP) above. Export once before editing text.csv.')

    def log(self,message):
        self.log_box.configure(state='normal')
        self.log_box.insert('end',str(message)+'\n');self.log_box.see('end')
        self.log_box.configure(state='disabled')

    def project_path(self):
        p=Path(self.project.get().strip(' "')).expanduser()
        if not p.exists():raise ValueError('Select an existing project folder or ZIP first.')
        return p.resolve()

    def get_dump(self):
        d=Path(self.dump.get().strip(' "')).expanduser()
        if not d.is_dir():
            picked=filedialog.askdirectory(title='Select exported text dump folder')
            if not picked:raise ValueError('No text dump selected')
            d=Path(picked);self.dump.set(str(d))
        return d.resolve()

    def _job(self,title,func,done=None):
        if self.busy:messagebox.showwarning('Busy','Wait for the current operation.');return
        self.busy=True;self.status.set(title+'...');self.log(title+'...')
        def runner():
            try:
                result=func()
                self.messages.put(('ok',title,result,done))
            except Exception as e:
                self.messages.put(('error',title,str(e)+'\n'+traceback.format_exc(limit=3),None))
        threading.Thread(target=runner,daemon=True).start()

    def process_messages(self):
        try:
            while True:
                status,title,value,done=self.messages.get_nowait();self.busy=False
                if status=='ok':
                    self.status.set(title+' — completed')
                    self.log('OK: '+str(value))
                    if done:
                        try:done(value)
                        except Exception as exc:messagebox.showerror('UI update error',str(exc))
                else:
                    self.status.set(title+' — FAILED')
                    self.log('ERROR: '+value)
                    messagebox.showerror('Operation failed',value.split('\n')[0])
        except queue.Empty:pass
        self.root.after(100,self.process_messages)

    def text_export(self):
        try:source=self.project_path()
        except Exception as exc:messagebox.showerror('Project',str(exc));return
        parent=filedialog.askdirectory(title='Choose a PARENT folder for NEW export directory')
        if not parent:return
        dump=Path(parent)/('Violet_Text_'+time.strftime('%Y%m%d_%H%M%S'))
        if dump.exists():messagebox.showerror('Export','Export folder already exists');return
        self._job('Export text',lambda: (export_game(source,dump,graphics=False),str(dump))[1],
                  lambda _:self.dump.set(str(dump)))

    def _table(self,dump):
        csv=dump/'text.csv'
        return str(csv) if csv.exists() else None

    def text_check(self):
        try:source=self.project_path(); dump=self.get_dump();table=self._table(dump)
        except Exception as exc:messagebox.showerror('Check',str(exc));return
        def run():
            report=import_game(source,dump,None,encoding='cp949',table=table,check=True)
            return json.dumps(report,ensure_ascii=False,indent=2)
        self._job('Check text',run)

    def text_import(self):
        try:source=self.project_path(); dump=self.get_dump();table=self._table(dump)
        except Exception as exc:messagebox.showerror('Import',str(exc));return
        if source.is_file():
            target=filedialog.asksaveasfilename(title='Save NEW project ZIP',defaultextension='.zip',filetypes=[('ZIP','*.zip')])
            if not target:return
            dest=Path(target)
        else:
            parent=filedialog.askdirectory(title='Choose PARENT folder for NEW imported project')
            if not parent:return
            dest=Path(parent)/('Violet_Imported_'+time.strftime('%Y%m%d_%H%M%S'))
        if dest.exists():messagebox.showerror('Import','Destination must not exist');return
        self._job('Import text',lambda:import_game(source,dump,dest,encoding='cp949',table=table))

    def _make_font_tab(self):
        f=self.font_page
        tools=ttk.Frame(f);tools.pack(fill='x',pady=(0,8))
        buttons=[('Load font',self.load_font),('Save + rebuild cache',self.save_font),
                 ('Copy glyph',self.copy_glyph),('Paste glyph',self.paste_glyph),
                 ('Undo',self.undo),('Clear pixels',self.clear_glyph)]
        for title,fn in buttons:ttk.Button(tools,text=title,command=fn).pack(side='left',padx=(0,5))
        ttk.Label(f,text='⚠ Width = bitmap edit width (1..atlas width). Advance = metadata only; renderer glyph mapping/spacing not patched.',foreground='#a54317').pack(anchor='w',pady=(0,8))
        body=ttk.Panedwindow(f,orient='horizontal');body.pack(fill='both',expand=True)
        side=ttk.Frame(body);mid=ttk.Frame(body);right=ttk.Frame(body)
        body.add(side,weight=1);body.add(mid,weight=3);body.add(right,weight=1)
        # Sidebar: atlas + provisional/manual glyph region list
        sel=ttk.Frame(side);sel.pack(fill='x')
        ttk.Label(sel,text='Atlas:').pack(side='left')
        a=ttk.Combobox(sel,textvariable=self.atlas_var,values=[str(i) for i in range(8)],state='readonly',width=5)
        a.pack(side='left',padx=8);a.bind('<<ComboboxSelected>>',self.select_atlas)
        ttk.Label(side,text='Glyph regions (manual)').pack(anchor='w',pady=(12,4))
        self.region_list=tk.Listbox(side,exportselection=False,height=13,font=('Consolas',10))
        self.region_list.pack(fill='both',expand=True)
        self.region_list.bind('<<ListboxSelect>>',self.select_region)
        rs=ttk.Frame(side);rs.pack(fill='x',pady=7)
        ttk.Button(rs,text='+ Region',command=self.add_region).pack(side='left')
        ttk.Button(rs,text='− Region',command=self.del_region).pack(side='left',padx=5)
        ttk.Button(side,text='Suggest runs (unsafe boundaries)',command=self.auto_regions).pack(fill='x')
        ttk.Label(side,text='Regions are manually defined and do NOT establish which Unicode character each shape represents.',wraplength=210).pack(anchor='w',pady=12)
        # Center pixel editor
        hdr=ttk.Frame(mid);hdr.pack(fill='x')
        ttk.Label(hdr,text='Pixel editor',font=('Arial',12,'bold')).pack(side='left')
        ttk.Radiobutton(hdr,text='Pencil',variable=self.eraser,value=False).pack(side='left',padx=(18,5))
        ttk.Radiobutton(hdr,text='Eraser',variable=self.eraser,value=True).pack(side='left')
        self.region_tip=tk.StringVar(value='Select a glyph region')
        ttk.Label(mid,textvariable=self.region_tip).pack(anchor='w',pady=4)
        canvasframe=ttk.Frame(mid);canvasframe.pack(fill='both',expand=True)
        self.canvas=tk.Canvas(canvasframe,background='#253245',highlightthickness=0,cursor='crosshair')
        self.canvas.pack(side='left',fill='both',expand=True)
        v=ttk.Scrollbar(canvasframe,orient='vertical',command=self.canvas.yview);v.pack(side='right',fill='y')
        h=ttk.Scrollbar(mid,orient='horizontal',command=self.canvas.xview);h.pack(fill='x')
        self.canvas.configure(yscrollcommand=v.set,xscrollcommand=h.set)
        self.canvas.bind('<Button-1>',lambda e:self.stroke_start(e,False))
        self.canvas.bind('<Button-3>',lambda e:self.stroke_start(e,True))
        self.canvas.bind('<B1-Motion>',lambda e:self.stroke_move(e,False))
        self.canvas.bind('<B3-Motion>',lambda e:self.stroke_move(e,True))
        self.canvas.bind('<ButtonRelease-1>',lambda e:self._end_draw())
        self.canvas.bind('<ButtonRelease-3>',lambda e:self._end_draw())
        # Right: editable region metrics and atlas strip overview
        ttk.Label(right,text='Glyph metrics',font=('Arial',12,'bold')).pack(anchor='w')
        frm=ttk.Frame(right);frm.pack(fill='x',pady=7)
        for row,(name,var) in enumerate([('Label',self.name),('Y origin',self.ry),('Height',self.rh),('Width px',self.rw),('Advance*',self.adv)]):
            ttk.Label(frm,text=name+':').grid(row=row,column=0,sticky='w',pady=3)
            ttk.Entry(frm,textvariable=var,width=11).grid(row=row,column=1,sticky='ew',pady=3)
        ttk.Button(right,text='Apply metrics',command=self.apply_metrics).pack(fill='x',pady=(0,12))
        ttk.Label(right,text='Atlas overview (click to locate Y)').pack(anchor='w')
        of=ttk.Frame(right);of.pack(fill='both',expand=True)
        self.overview=tk.Canvas(of,background='#293444',width=120,highlightthickness=0)
        self.overview.pack(side='left',fill='both',expand=True)
        vs=ttk.Scrollbar(of,orient='vertical',command=self.overview.yview);vs.pack(side='right',fill='y')
        self.overview.configure(yscrollcommand=vs.set)
        self.overview.bind('<Button-1>',self.click_overview)
        ttk.Label(right,text='* Advance stored in JSON only.',wraplength=180).pack(anchor='w',pady=7)

    def require_font(self):
        if self.busy:raise ValueError('Wait for the active task to finish')
        if self.doc is None:raise ValueError('Load font first')
        return self.doc

    def font_root(self):
        p=self.project_path()
        if not p.is_dir():raise ValueError('Font editor needs an extracted project folder, not ZIP.')
        if (p/'nitrofs'/'res'/'font').is_file():return p
        if p.name=='nitrofs' and (p/'res'/'font').is_file():return p.parent
        raise FileNotFoundError('Project missing nitrofs/res/font')

    def load_font(self):
        try:
            p=self.font_root()
            self.doc=FontDocument(p/'nitrofs'/'res'/'font',p/'translation'/'font_glyph_regions.json')
            self.current_atlas=0;self.atlas_var.set('0');self.current_reg=0
            self.populate_regions();self.refresh_overview();self.status.set('Font loaded: 8 indexed PNG strips, glyph segmentation remains manual.')
        except Exception as exc:messagebox.showerror('Load font',str(exc))

    def select_atlas(self,event=None):
        if not self.doc:return
        self.current_atlas=int(self.atlas_var.get());self.current_reg=0
        self.populate_regions();self.refresh_overview()

    def populate_regions(self):
        self.region_list.delete(0,'end')
        if not self.doc:return
        regions=self.doc.regions[self.current_atlas]
        for i,r in enumerate(regions):
            self.region_list.insert('end',f'{i:03d} y={r["y"]:>3} h={r["height"]:>3} {r["name"][:12]}')
        if regions:
            self.current_reg=min(self.current_reg,len(regions)-1)
            self.region_list.selection_set(self.current_reg)
            self.region_list.see(self.current_reg)
            self.show_region()
        else:self.canvas.delete('all');self.region_tip.set('No regions. Add one.')

    def select_region(self,event=None):
        if not self.doc:return
        s=self.region_list.curselection()
        if not s:return
        self.current_reg=s[0];self.show_region()

    def selected(self):
        doc=self.require_font()
        if not doc.regions[self.current_atlas]:raise ValueError('Create a region first')
        return doc,doc.regions[self.current_atlas][self.current_reg]

    def show_region(self):
        try:doc,r=self.selected()
        except Exception:return
        self.name.set(r['name']);self.ry.set(str(r['y']));self.rh.set(str(r['height']))
        self.rw.set(str(r['width']));self.adv.set(str(r['advance']))
        self.region_tip.set(f'Atlas {self.current_atlas}, Y={r["y"]}…{r["y"]+r["height"]-1}; left draw, right erase')
        self.render_canvas();self.refresh_overview()

    def render_canvas(self):
        self.canvas.delete('all');self.cells={}
        try:doc,r=self.selected()
        except Exception:return
        z=23; w=doc.images[self.current_atlas].width;h=r['height']
        self.canvas.configure(scrollregion=(0,0,w*z+10,h*z+10))
        for y in range(h):
            for x in range(w):
                v=doc.images[self.current_atlas].getpixel((x,r['y']+y))
                color='#fbe5b1' if v else '#28374b'
                if x>=r['width']:color='#493947' if not v else '#b09a78'
                item=self.canvas.create_rectangle(x*z+3,y*z+3,(x+1)*z+2,(y+1)*z+2,fill=color,outline='#657385',width=1)
                self.cells[x,y]=item
        self.canvas.create_line(r['width']*z+2,0,r['width']*z+2,h*z,fill='#ff8e54',width=2,tags='limit')

    def _cell_from_event(self,event):
        x=int((self.canvas.canvasx(event.x)-3)//23);y=int((self.canvas.canvasy(event.y)-3)//23)
        return x,y

    def stroke_start(self,event,erase):
        try:doc,r=self.selected()
        except Exception:return
        doc.snapshot();self.drawing=True
        self._paint(event,erase)

    def stroke_move(self,event,erase):
        if self.drawing:self._paint(event,erase)

    def _end_draw(self):self.drawing=False

    def _paint(self,event,erase):
        try:doc,r=self.selected()
        except Exception:return
        x,y=self._cell_from_event(event)
        actual_erase=erase or self.eraser.get()
        if doc.stroke(self.current_atlas,self.current_reg,x,y,0 if actual_erase else 1):
            item=self.cells.get((x,y))
            if item:self.canvas.itemconfigure(item,fill='#28374b' if actual_erase else '#fbe5b1')

    def refresh_overview(self):
        self.overview.delete('all')
        if not self.doc:return
        img=self.doc.images[self.current_atlas];scale_x=10;scale_y=3
        self.overview.configure(scrollregion=(0,0,img.width*scale_x+12,img.height*scale_y+5))
        for y in range(img.height):
            for x in range(img.width):
                v=img.getpixel((x,y))
                self.overview.create_rectangle(x*scale_x+10,y*scale_y,(x+1)*scale_x+10,(y+1)*scale_y,
                   fill='#fbe5b1' if v else '#334253',outline='')
        if self.doc.regions[self.current_atlas]:
            r=self.doc.regions[self.current_atlas][self.current_reg]
            self.overview.create_rectangle(1,r['y']*scale_y, img.width*scale_x+11,(r['y']+r['height'])*scale_y,
                 outline='#ff914d',width=2)

    def click_overview(self,event):
        if not self.doc:return
        y=int(self.overview.canvasy(event.y)//3)
        for i,r in enumerate(self.doc.regions[self.current_atlas]):
            if r['y']<=y<r['y']+r['height']:
                self.current_reg=i;self.region_list.selection_clear(0,'end');self.region_list.selection_set(i)
                self.region_list.see(i);self.show_region();return
        self.ry.set(str(y))
        self.status.set(f'Atlas Y={y}. Use + Region to define a new glyph span.')

    def apply_metrics(self):
        try:
            doc,r=self.selected(); fields={'name':self.name.get(),'y':int(self.ry.get()),
                       'height':int(self.rh.get()),'width':int(self.rw.get()),'advance':int(self.adv.get())}
            doc.update_region(self.current_atlas,self.current_reg,**fields)
            self.populate_regions()
        except Exception as exc:messagebox.showerror('Metrics',str(exc))

    def add_region(self):
        try:
            doc=self.require_font();a=self.current_atlas
            proposed=int(self.ry.get()) if self.ry.get().isdigit() else 0
            y=simpledialog.askinteger('New glyph region','Start row (Y):',initialvalue=proposed,minvalue=0,maxvalue=doc.images[a].height-1)
            if y is None:return
            height=simpledialog.askinteger('New glyph region','Region height:',initialvalue=10,minvalue=1,maxvalue=doc.images[a].height-y)
            if height is None:return
            self.current_reg=doc.add_region(a,y,height)
            self.populate_regions()
        except Exception as exc:messagebox.showerror('Add region',str(exc))

    def del_region(self):
        try:
            doc,_=self.selected()
            if not messagebox.askyesno('Delete region','Delete this region definition? Pixel data will remain unchanged.'):return
            doc.delete_region(self.current_atlas,self.current_reg)
            self.current_reg=0;self.populate_regions();self.refresh_overview()
        except Exception as exc:messagebox.showerror('Delete region',str(exc))

    def auto_regions(self):
        try:
            doc=self.require_font()
            if not messagebox.askyesno('Unverified glyph boundaries',
                  'Replace ALL region definitions in this atlas with tentative non-empty raster runs?\n'
                  'Blank lines inside a glyph may create false splits. Use Undo to reverse.'):
                return
            count=doc.detect_candidates(self.current_atlas);self.current_reg=0
            self.populate_regions();self.status.set(f'{count} candidate regions: MUST verify manually.')
        except Exception as exc:messagebox.showerror('Suggest regions',str(exc))

    def copy_glyph(self):
        try:
            doc,_=self.selected();data=doc.copy_glyph(self.current_atlas,self.current_reg)
            self.root.clipboard_clear();self.root.clipboard_append(CLIP_PREFIX+json.dumps(data,ensure_ascii=False))
            self.root.update_idletasks()
            self.status.set(f'Copied glyph bitmap + label, width={data["width"]}, height={data["height"]}, advance={data["advance"]}.')
        except Exception as exc:messagebox.showerror('Copy glyph',str(exc))

    def paste_glyph(self):
        try:
            doc,_=self.selected();v=self.root.clipboard_get()
            if not v.startswith(CLIP_PREFIX):raise ValueError('Clipboard does not contain a Violet glyph')
            data=json.loads(v[len(CLIP_PREFIX):])
            doc.paste_glyph(self.current_atlas,self.current_reg,data)
            self.populate_regions();self.status.set('Pasted pixels and ALL copied glyph metrics.')
        except Exception as exc:messagebox.showerror('Paste glyph',str(exc))

    def clear_glyph(self):
        try:
            doc,_=self.selected();doc.clear_region(self.current_atlas,self.current_reg)
            self.render_canvas();self.refresh_overview()
        except Exception as exc:messagebox.showerror('Clear',str(exc))

    def undo(self):
        try:
            if self.require_font().undo():self.current_reg=0;self.populate_regions();self.refresh_overview()
            else:self.status.set('Undo history empty')
        except Exception as exc:messagebox.showerror('Undo',str(exc))

    def save_font(self):
        try:
            doc=self.require_font();p=self.font_root()
            if doc.font_path.resolve() != (p/'nitrofs'/'res'/'font').resolve():
                raise ValueError('Project path changed after loading font; please Load font again.')
            # No mutation of document from Tk while background cache builds.
            packed=doc.packed();meta=doc.regions_bytes()
        except Exception as exc:messagebox.showerror('Save font',str(exc));return
        if not messagebox.askyesno('Save and rebuild cache',
            'Write res/font + glyph region metadata + both PNG cache files?\n'
            'Existing files get .violetbak backups. You must rebuild ROM afterwards.\n\n'
            'Advance-width metadata does NOT patch the runtime renderer.'):
            return
        def run():
            # Font core remains immutable during this operation via busy guard.
            return commit_font(doc,p)
        def finish(out):
            messagebox.showinfo('Font saved',
                f'Updated {out["written"]} files; {out["unique_pngs"]} unique PNG cache entries.\n'
                'Build a NEW .nds ROM to test. Advance values are only editor metadata.')
        self._job('Save font and rebuild PNG cache',run,finish)


def main():
    root=tk.Tk();App(root);root.mainloop()


if __name__ == '__main__':
    main()

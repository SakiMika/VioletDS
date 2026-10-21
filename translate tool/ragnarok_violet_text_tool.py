#!/usr/bin/env python3
"""Ragnarok Violet WIPI text export/import. Python standard library only."""
from __future__ import annotations
import argparse, collections, csv, hashlib, json, os, re, shutil, struct, sys, tempfile, unicodedata, zipfile
from pathlib import Path, PurePosixPath

VERSION=1
TOOL='ragnarok-violet-text'
VI_ORDER='áàảãạâấầẩẫậăắằẳẵặđéèẻẽẹêếềểễệíìỉĩịóòỏõọôốồổỗộơớờởỡợúùủũụưứừửữựýỳỷỹỵ'
VI_MAP={ch:0x80+i for i,ch in enumerate(VI_ORDER)}
# Verified code/rodata boundary for the supplied Ragnarok ELF. Never scan Thumb
# instructions as strings. Other revisions still get structured resource export.
PROFILE_SHA='4aff4641efeff7578afc6d02c44b5c45279648200bb5af6e9db1996778ec609a'
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

# JSON strings hold actual newlines; byte escapes are explicit \\xHH. No lossy decode.
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
    rows=[];profile=sha(b)==PROFILE_SHA
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
    if manifest.get('files')!=files or manifest.get('binary_sha256')!=sha(game.read('binary.mod')):raise TextError('Source changed since export')
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

if __name__=='__main__':raise SystemExit(main())

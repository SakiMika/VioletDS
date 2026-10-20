from pathlib import Path
import tempfile,subprocess,hashlib,json
r=Path(__file__).resolve().parents[1]
rom=(r/'prebuilt/Zenonia_TEST6_NOSTRIPE_DS.nds').read_bytes()
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'sample.nds';p.write_bytes(rom)
 for _ in range(2):subprocess.run(['python3',str(r/'tools/patch_nds_header.py'),str(p)],check=True)
 assert p.read_bytes()==rom,'Header script must be idempotent on TEST6'
assert '-g "####" 01' in (r/'Makefile').read_text()
for p in (r/'.github/workflows').glob('*'):
 if p.is_file():assert "b'AAFA'" not in p.read_text()
manifest=json.loads((r/'validation/NITROFS_SHA256.json').read_text())
for f,h in manifest.items():assert hashlib.sha256((r/f).read_bytes()).hexdigest()==h,f
print('PASS TEST6 homebrew header/CRC script idempotence, build/CI identity and all NitroFS hashes')

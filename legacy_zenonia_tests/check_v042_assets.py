"""Verify all guest assets against the user-supplied v041 ZIP, not the older manifest."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[1]
manifest=json.loads((root/'tests/v042-input-assets.json').read_text())
actual={str(p.relative_to(root)) for d in ['nitrofs','nitrofs bak'] for p in (root/d).rglob('*') if p.is_file()}
derived={'nitrofs/runner_audio/title108.zpc'}
assert actual==set(manifest)|derived,'Asset file set changed'
cache=root/next(iter(derived))
meta=json.loads((root/'assets/audio/title108-manifest.json').read_text())
assert hashlib.sha256(cache.read_bytes()).hexdigest()==meta['cache_sha256']
for name,digest in manifest.items():
 assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
print(f'PASS all {len(manifest)} guest asset files identical to supplied ZIP')

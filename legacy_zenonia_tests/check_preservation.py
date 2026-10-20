#!/usr/bin/env python3
"""Verify unchanged guest binary, GUI resources and bitmap fonts against manifest."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[1]
audit=json.loads((root/'tests/preservation_manifest.json').read_text())
for name,digest in audit.items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
print('PASS: unchanged guest binary, GUI and bitmap fonts')

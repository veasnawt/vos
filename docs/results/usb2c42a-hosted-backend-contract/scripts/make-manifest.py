#!/usr/bin/env python3
"""Regenerate manifest.json (source, trace and evidence hashes) from the current tree. Run from the repository root."""
import hashlib, json
from pathlib import Path
ROOT = Path('/home/veasnawt/src/vos-usb2c42a')
E = ROOT / 'docs/results/usb2c42a-hosted-backend-contract'
def h(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
m = json.loads((E / 'manifest.json').read_text())
m['source_sha256'] = [f"{h(ROOT / l.split('  ', 1)[1])}  {l.split('  ', 1)[1]}" for l in m['source_sha256']]
m['oracle_trace_sha256'] = f"sha256 of build/usb2c42a-1/gates-4/trace.txt: {h(ROOT / 'build/usb2c42a-1/gates-4/trace.txt')} (kept in build/, not in the repository)"
skip = {'manifest.json', 'secret-audit.json'}
m['evidence_sha256'] = [f'{h(f)}  {f.relative_to(E)}' for f in sorted(E.rglob('*')) if f.is_file() and f.name not in skip or (f.is_file() and f.parent != E and f.name in skip)]
(E / 'manifest.json').write_text(json.dumps(m, indent=1) + '\n')
print(len(m['evidence_sha256']), 'evidence files')

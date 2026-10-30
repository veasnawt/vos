#!/usr/bin/env python3
"""Write manifest.json: source, image and evidence hashes. Format `<sha256>  <path>` (no file name sits next to a hash in JSON
`"name": "hash"` form, which a generic-key secret rule would flag). Run from the repository root."""
import hashlib, json, subprocess
from pathlib import Path
ROOT = Path('/home/veasnawt/src/vos-usb-inc1')
B = ROOT / 'build/inc1-1'
E = ROOT / 'docs/results/usb-inc1-hosted-safety-and-survey'
def h(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
changed = subprocess.run(['git', 'diff', '--name-only', 'c909a7f'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
changed += subprocess.run(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
changed = sorted(set(f for f in changed if not f.startswith('docs/results/usb-inc1-hosted-safety-and-survey/') and (ROOT / f).is_file()))
rustc = subprocess.run(['/home/veasnawt/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc', '-vV'], capture_output=True, text=True).stdout.splitlines()[:3]
m = {
    'baseline': 'c909a7f334809080fae47377ff8c3b1e3f3b51e3 (merge of PR #35)',
    'authorization': 'owner message of 2026-10-30: prepare Increment 1 (hosted safety fixes and read-only discovery), stop after verification; no controller reset, PCI write, DMA activation or physical test',
    'storage_acceptance': 'owner confirmation (2026-10-30) that Antigravity accepted forecast 180000000, stop line 243000000, cap 270000000 bytes; stop line later raised to 262000000 with owner approval after storage investigation 2; see storage/',
    'changed_source_sha256': [f'{h(ROOT / f)}  {f}' for f in changed],
    'images': {
        'production_kernel': f"{h(B / 'kernel/vos.bin')}  build/inc1-1/kernel/vos.bin",
        'isos': [f"{h(p)}  {p.relative_to(ROOT)}" for p in sorted((B / 'kernel').glob('*.iso'))],
        'regression_isos': [f"{h(p)}  {p.relative_to(ROOT)}" for p in sorted((B / 'reg').glob('*.iso'))],
    },
    'oracle_trace_sha256': {
        'c41_machine_gate': f"{h(B / 'gate41-1/trace.txt')}  build/inc1-1/gate41-1/trace.txt (kept in build/)",
        'c42a_session_gate': f"{h(B / 'gate42-2/trace.txt')}  build/inc1-1/gate42-2/trace.txt (kept in build/)",
    },
    'toolchain': {'rustc': rustc, 'qemu': subprocess.run(['qemu-system-x86_64', '--version'], capture_output=True, text=True).stdout.splitlines()[0],
                  'ovmf_code': h('/usr/share/OVMF/OVMF_CODE_4M.fd'), 'python': '3 (local); python3 on ubuntu-24.04 in CI'},
    'evidence_sha256_format': '<sha256>  <path relative to this directory> (manifest.json and secret-audit.json excluded)',
    'evidence_sha256': [f'{h(f)}  {f.relative_to(E)}' for f in sorted(E.rglob('*')) if f.is_file() and f.parent != E or (f.is_file() and f.name not in ('manifest.json', 'secret-audit.json'))],
}
(E / 'manifest.json').write_text(json.dumps(m, indent=1) + '\n')
print(len(m['evidence_sha256']), 'evidence files,', len(changed), 'changed sources')

#!/usr/bin/env python3
"""Copies the R1-R3 verification outputs into docs/results/usb-inc2-survey-v2/verification-r1-r3/ and writes its own manifest.json.
The original 94-file manifest (docs/results/usb-inc2-survey-v2/manifest.json) is never read for writing or touched. Never deletes."""
import hashlib, json, shutil, subprocess, sys
from pathlib import Path
R = Path('/home/veasnawt/src/vos-usb-inc2'); B = R / 'build/i2-1'; D = R / 'docs/results/usb-inc2-survey-v2/verification-r1-r3'
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def put(src, dst):
    dst = D / dst; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(src, dst)
if sys.argv[1] == 'copy':
    for n in ('result.json', 'usb-resources-build.txt', 'usb-resources-run.txt', 'usb2c1-qualify-build.txt', 'usb2c1-qualify-run.txt', 'usb2c3a-residue-build.txt', 'usb2c3a-residue-run.txt'):
        put(B / 'r1' / n, f'r1/{n}')
    for run in ('r3-1', 'r3-2'):
        for n in ('result.json', 'sanitizer.txt', 'sanitizer-build.txt'):
            put(B / run / n, f'{run}/{n}')
    # .gitignore excludes *.log: committed evidence logs are copied under a .txt name (content byte-identical)
    for n, dst in (('serial.log', 'serial.txt'), ('serial.log.qemu', 'serial.log.qemu'), ('test-sh.txt', 'test-sh.txt')):
        put(B / 'r2a' / n, f'r2a/{dst}')
    for case in ('boot-128M', 'boot-2G'):
        for n, dst in (('serial.log', 'serial.txt'), ('qemu.log', 'qemu.txt'), ('qemu.stderr', 'qemu.stderr')):
            put(B / 'test-kernel' / f'{case}.{n}', f'r2b/{case}.{dst}')
    for n in ('r2b-build.txt', 'r2b-run-128M.txt', 'r2b-run-2G.txt', 'r2b-run.sh', 'r1.py', 'storage-log.txt', 'assemble2.py'):
        put(B / 'final' / n, f'scripts-and-logs/{n}')
else:
    files = {str(p.relative_to(D)): sha(p) for p in sorted(D.rglob('*')) if p.is_file() and p.name != 'manifest.json'}
    tracked = ['tests/host/usb-firmware-lease.c', 'scripts/test-usb-firmware-lease.py']
    head = subprocess.run(['git', '-C', str(R), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    old = Path(R / 'docs/results/usb-inc2-survey-v2/manifest.json')
    (D / 'manifest.json').write_text(json.dumps({
        'base_commit': head, 'worktree_changes_uncommitted': True, 'physical_hardware': False,
        'original_manifest_sha256_unchanged': sha(old),
        'report_sha256_in_original_manifest_at_210d25f': json.loads(old.read_text())['files']['../usb-inc2-survey-v2.md'],
        'report_sha256_now': sha(R / 'docs/results/usb-inc2-survey-v2.md'),
        'new_sources_sha256': {t: sha(R / t) for t in tracked},
        'not_copied_hashes': {'test-kernel/vos.bin': sha(B / 'test-kernel/vos.bin'), 'test-kernel/vos.iso': sha(B / 'test-kernel/vos.iso'),
                              'kernel/vos.iso (production BIOS, unchanged)': sha(B / 'kernel/vos.iso'), 'kernel/vos.bin (production, unchanged)': sha(B / 'kernel/vos.bin')},
        'files': files}, indent=1) + '\n')
    print(len(files), 'files')

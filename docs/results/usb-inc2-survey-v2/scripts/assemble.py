#!/usr/bin/env python3
"""Copies the Increment 2 evidence into docs/results/usb-inc2-survey-v2/ and writes manifest.json (SHA-256 of every file,
plus hashes of the kernel and ISOs, which are not copied). Read-only with respect to build/; never deletes."""
import hashlib, json, shutil, subprocess, sys
from pathlib import Path
R = Path('/home/veasnawt/src/vos-usb-inc2'); B = R / 'build/i2-1'; D = R / 'docs/results/usb-inc2-survey-v2'
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def put(src, dst):
    dst = D / dst; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(src, dst)
if sys.argv[1] == 'copy':
    for g in ('survey-gate-1', 'survey-gate-2', 'gate41-1', 'gate42-1'):
        put(B / g / 'result.json', f'gates/{g}-result.json')
        for n in ('survey-tests.txt', 'firmware-tests.txt', 'tests.txt', 'oracle.txt'):
            if (B / g / n).exists(): put(B / g / n, f'gates/{g}-{n}')
    put(B / 'survey-gate-1.stdout', 'gates/survey-gate-1.stdout')
    for n in ('gate41-1.stdout', 'gate42-1.stdout'): put(B / n, f'gates/{n}')
    for run in ('native-1', 'native-2', 'native-3'):
        put(B / run / 'result.json', f'native/{run}-result.json')
        for case in sorted(p for p in (B / run).iterdir() if p.is_dir()):
            put(case / 'serial.txt', f'native/{run}/{case.name}/serial.txt')
            put(case / 'command.json', f'native/{run}/{case.name}/command.json')
    for n in sorted(B.glob('native-*/*/screen.png')):
        put(n, f'screenshots/{n.parent.parent.name}-{n.parent.name}.png'.replace('keyboard', 'kbd'))
    put(B / 'native-1.stdout', 'native/native-1.stdout')
    put(B / 'regression-1/result.json', 'regression/result.json')
    for case in ('n', 'q', 'd', 'c'):
        put(B / 'regression-1' / case / 'serial.txt', f'regression/{case}-serial.txt')
    put(B / 'regression-1/grub.cfg', 'regression/grub.cfg')
    for n in sorted((B / 'final').iterdir()):
        if n.is_file() and n.name not in ('leak-scan.py', 'assemble.py') and n.stat().st_size < 400000: put(n, f'storage/{n.name}')
    put(B / 'final/leak-scan.py', 'scripts/leak-scan.py'); put(B / 'final/assemble.py', 'scripts/assemble.py')
else:
    files = {str(p.relative_to(D)): sha(p) for p in sorted(D.rglob('*')) if p.is_file() and p.name != 'manifest.json'}
    files['../usb-inc2-survey-v2.md'] = sha(R / 'docs/results/usb-inc2-survey-v2.md')
    big = {n: sha(B / 'kernel' / n) for n in ('vos.bin', 'vos.iso', 'vos-uefi.iso', 'vos-usb-survey.iso', 'vos-uefi-usb-survey.iso')}
    big['regression-1/regression-bios.iso'] = sha(B / 'regression-1/regression-bios.iso')
    head = subprocess.run(['git', '-C', str(R), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    (D / 'manifest.json').write_text(json.dumps({'base_commit': head, 'worktree_changes_uncommitted': True, 'physical_hardware': False,
        'not_copied_hashes': big, 'files': files}, indent=1) + '\n')
    print(len(files), 'files')

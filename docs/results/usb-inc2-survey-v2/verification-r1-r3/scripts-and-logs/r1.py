#!/usr/bin/env python3
"""R1: the sanitizer subset of scripts/test-usb-resources.py, test-usb2c1.py and test-usb2c3a.py (same gcc flags, same sources,
existing kernel archive). Nothing else from those scripts (Rust tests, mutation trees, benchmark builds) is run. Never retries; every
log is kept, including a failed build."""
import hashlib, json, subprocess, sys
from pathlib import Path
R = Path('/home/veasnawt/src/vos-usb-inc2'); out = R / 'build/i2-1/r1'; out.mkdir()
archive = R / 'build/i2-1/kernel/rust/libvos_kernel.a'
base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
rep = {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'cases': {}, 'commands': []}
ok = True
for name, src in (('usb-resources', 'tests/host/usb-resources.c'), ('usb2c1-qualify', 'tests/host/usb2c1-qualify.c'), ('usb2c3a-residue', 'tests/host/usb2c3a-residue.c')):
    cmd = base + san + [src, str(archive), '-o', str(out / f'{name}-sanitizer')]
    rep['commands'].append(cmd)
    with (out / f'{name}-build.txt').open('x') as f:
        b = subprocess.run(cmd, cwd=R, stdout=f, stderr=subprocess.STDOUT)
    case = {'build_rc': b.returncode}
    if b.returncode == 0:
        with (out / f'{name}-run.txt').open('x') as f:
            r = subprocess.run([str(out / f'{name}-sanitizer')], cwd=R, stdout=f, stderr=subprocess.STDOUT, timeout=300)
        case['run_rc'] = r.returncode
        case['last_line'] = (out / f'{name}-run.txt').read_text().strip().splitlines()[-1:] 
        ok &= r.returncode == 0
    else:
        ok = False
    rep['cases'][name] = case
    print(name, case, flush=True)
    if not ok: break
rep['pass'] = ok
(out / 'result.json').write_text(json.dumps(rep, indent=1) + '\n')
sys.exit(0 if ok else 1)

#!/usr/bin/env python3
"""Focused USB.2-B real-service hosted tests; fresh output, no packages/devices.

Supply a kernel archive built from this checkout with the normal kernel flags.
Rust machine code is not ASan-instrumented; C adapters/PMM/devmap are.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'commands': [], 'rust_sanitized': False, 'native_hardware': False,
              'archive_sha256': hashlib.sha256(args.archive.read_bytes()).hexdigest()}

    def run(argv, name, expected=0):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                               timeout=120, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        if p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (out / name).read_text()

    try:
        base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
                '-fdata-sections', '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
        sources = ['tests/host/usb-resources.c', args.archive]
        run(base + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] +
            sources + ['-o', out/'sanitizer'], 'sanitizer-build.txt')
        report['sanitizer'] = run([out/'sanitizer'], 'sanitizer.txt').strip()
        run(base + ['-O2', '-DUSB2B_BENCH'] + sources + ['-o', out/'release'], 'release-build.txt')
        report['release'] = run([out/'release'], 'release.txt').strip()
        module = ROOT/'arch/x86_64/rust/resource_ownership.rs'
        run([args.rustc, '--edition=2024', '-Dwarnings', '--test', module, '-o', out/'rust-tests'],
            'rust-build.txt')
        report['rust'] = run([out/'rust-tests'], 'rust-tests.txt').strip()
        for bound in ('Copy', 'Send', 'Sync'):
            src = out/(bound.lower()+'.rs')
            src.write_text(f'#[path="{module}"] mod resource_ownership;\n'
                           f'fn require<T: {bound}>() {{}}\n'
                           'fn main(){require::<resource_ownership::Owned>();}\n')
            log = run([args.rustc, '--edition=2024', '-Dwarnings', src, '-o', out/bound],
                      bound.lower()+'-negative.txt', 1)
            if 'E0277' not in log:
                raise RuntimeError(bound+' control failed for unrelated reason')
        report['ownership_compile_rejections'] = ['Copy', 'Send', 'Sync']
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()

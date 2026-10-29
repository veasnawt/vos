#!/usr/bin/env python3
"""USB.2-C1 pure Rust and hosted C/Rust qualification tests; fresh output only.

Supply a kernel archive built from this checkout with the normal kernel flags.
Rust machine code is not sanitizer-instrumented; the C PMM/devmap/USB
resource/qualification code and the harness are. Mutation controls run on a
temporary copy of the canonical tree; sources are never modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'kernel/rust/usb/xhci'
MUTATIONS = {
    # Disable PCI capability cycle detection: a looping list must be caught.
    'pci-cycle': ('qualify.rs', 'if seen & bit != 0 {', 'if false && seen & bit != 0 {',
                  'capability_list_offsets_cycles_and_limit'),
    # Accept duplicate legacy-support capabilities.
    'duplicate-legacy': ('capabilities.rs', 'if legacy.is_some() {', 'if false && legacy.is_some() {',
                         'duplicate_and_malformed_extended_capabilities'),
}


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
    archive = args.archive.resolve()
    report = {'commands': [], 'rust_sanitized': False, 'hardware': False,
              'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'source_sha256': {}}
    for p in sorted(CORE.glob('*.rs')):
        text = p.read_text()
        if re.search(r'\bunsafe\b|\bextern\b|\basm!|\balloc::|\bstd::', text):
            raise SystemExit('nonportable/unsafe core source: ' + str(p))
        report['source_sha256'][str(p.relative_to(ROOT))] = hashlib.sha256(p.read_bytes()).hexdigest()

    def run(argv, name, expected=0):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=300,
                               env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        if p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (out / name).read_text()

    rust = [args.rustc, '--edition=2024', '-Dwarnings']
    try:
        lib = out / 'libvos_xhci.rlib'
        run(rust + ['--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes',
                    '-Cdebug-assertions=yes', CORE / 'lib.rs', '-o', lib], 'core-build.txt')
        run(rust + ['--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_xhci={lib}',
                    'tests/rust/xhci_qualify.rs', '-o', out / 'qualify-tests'], 'qualify-build.txt')
        log = run([out / 'qualify-tests', '--test-threads=1'], 'qualify-tests.txt')
        report['qualify_tests'] = re.search(r'test result: .*', log)[0]
        report['mutation_controls'] = {}
        for name, (file, old, new, test) in MUTATIONS.items():
            tree = out / ('mutant-' + name)
            shutil.copytree(CORE, tree)
            src = (tree / file).read_text()
            if src.count(old) != 1:
                raise RuntimeError('mutation anchor not unique: ' + name)
            (tree / file).write_text(src.replace(old, new))
            mlib = tree / 'libvos_xhci.rlib'
            run(rust + ['--crate-name', 'vos_xhci', '--crate-type=rlib', tree / 'lib.rs', '-o', mlib],
                f'mutant-{name}-build.txt')
            run(rust + ['--test', '--extern', f'vos_xhci={mlib}', 'tests/rust/xhci_qualify.rs',
                        '-o', tree / 'tests'], f'mutant-{name}-test-build.txt')
            mlog = run([tree / 'tests', '--test-threads=1', test], f'mutant-{name}-tests.txt', 101)
            if f'{test} ... FAILED' not in mlog:
                raise RuntimeError('mutation control failed for an unrelated reason: ' + name)
            report['mutation_controls'][name] = f'detected by {test} (exit 101)'
        base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
                '-fdata-sections', '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
        sources = ['tests/host/usb2c1-qualify.c', archive]
        run(base + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] + sources +
            ['-o', out / 'sanitizer'], 'sanitizer-build.txt')
        report['sanitizer'] = run([out / 'sanitizer'], 'sanitizer.txt').strip()
        run(base + ['-O2', '-DUSB2C1_BENCH'] + sources + ['-o', out / 'release'], 'release-build.txt')
        timing = run([out / 'release'], 'release.txt')
        samples = sorted(int(x) for x in re.findall(r'ns_per_qualification=(\d+)', timing))
        report['release_ns_per_qualification'] = {'samples_sorted': samples, 'median': samples[len(samples) // 2]}
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

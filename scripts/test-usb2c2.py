#!/usr/bin/env python3
"""USB.2-C2 pure Rust and hosted C/Rust DMA-ownership tests; fresh output only.

Supply a kernel archive built from this checkout with the normal kernel flags.
Rust machine code is not sanitizer-instrumented; the PMM, USB DMA adapter and
harness C are. Mutation controls edit a temporary copy of the canonical tree.
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
    'no-generation-check': ('&& token(slot, r.generation) == page.lease.token()\n', '\n',
                            'stale_and_foreign_handles_are_refused'),
    'unbounded-quarantine': ('if quarantined < QUARANTINE_LIMIT && !self.closed {', 'if quarantined < 255 {',
                             'quarantine_is_bounded_and_fails_closed'),
    'no-rollback': ('                    self.cancel(f, out);\n', '', 'partial_set_failures_at_every_step'),
}
# Each must fail to compile with the named error: ownership rules hold statically.
FRAMES = ('struct F; impl Frames for F { fn alloc(&mut self) -> Result<u64, Error> { Ok(0x20_0000) }\n'
          ' fn zero(&mut self, _: u64) -> Result<(), Error> { Ok(()) }\n'
          ' fn free(&mut self, _: u64) -> Result<(), Error> { Ok(()) } }\n')
PRELUDE = ('use vos_xhci::xhci::Error;\nuse vos_xhci::xhci::dma::*;\nuse vos_xhci::xhci::resources::Kind;\n'
           '#[allow(dead_code)] fn require<T: ?Sized>() {}\n')
COMPILE_FAIL = {
    'page-copy': ('fn needs<T: Copy>() {}\nfn main() { needs::<Page>(); }\n', 'E0277'),
    'page-send': ('fn needs<T: Send>() {}\nfn main() { needs::<Page>(); }\n', 'E0277'),
    'page-sync': ('fn needs<T: Sync>() {}\nfn main() { needs::<Page>(); }\n', 'E0277'),
    'double-release': (FRAMES + 'fn main() { let mut p = Pool::new(1 << 32); let mut f = F;\n'
                       ' let page = p.allocate(&mut f, Kind::Command).unwrap();\n'
                       ' let _ = p.release(&mut f, page); let _ = p.release(&mut f, page); }\n', 'E0382'),
    'raw-frame-access': (FRAMES + 'fn main() { let mut p = Pool::new(1 << 32); let mut f = F;\n'
                         ' let page = p.allocate(&mut f, Kind::Command).unwrap(); let _ = page.lease; }\n', 'E0616'),
    'publish-proof': ('fn main() { let _ = PublishProof::Granted; }\n', 'E0599'),
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
              'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'source_sha256': {}}
    for p in sorted(CORE.glob('*.rs')):
        if re.search(r'\bunsafe\b|\bextern\b|\basm!|\balloc::|\bstd::', p.read_text()):
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
                    'tests/rust/xhci_dma.rs', '-o', out / 'dma-tests'], 'dma-build.txt')
        report['dma_tests'] = re.search(r'test result: .*', run([out / 'dma-tests', '--test-threads=1'],
                                                                 'dma-tests.txt'))[0]
        report['compile_fail_controls'] = {}
        for name, (body, code) in COMPILE_FAIL.items():
            src = out / f'cf-{name}.rs'
            src.write_text(PRELUDE + body)
            log = run([args.rustc, '--edition=2024', '--extern', f'vos_xhci={lib}', src, '-o', out / f'cf-{name}'],
                      f'cf-{name}.txt', 1)
            if code not in log:
                raise RuntimeError(f'compile-fail control {name} failed for an unrelated reason')
            report['compile_fail_controls'][name] = f'rejected with {code}'
        report['mutation_controls'] = {}
        for name, (old, new, test) in MUTATIONS.items():
            tree = out / ('mutant-' + name)
            shutil.copytree(CORE, tree)
            src = (tree / 'dma.rs').read_text()
            if src.count(old) != 1:
                raise RuntimeError('mutation anchor not unique: ' + name)
            (tree / 'dma.rs').write_text(src.replace(old, new))
            mlib = tree / 'libvos_xhci.rlib'
            run([args.rustc, '--edition=2024', '--crate-name', 'vos_xhci', '--crate-type=rlib', tree / 'lib.rs',
                 '-o', mlib], f'mutant-{name}-build.txt')
            run([args.rustc, '--edition=2024', '--test', '--extern', f'vos_xhci={mlib}', 'tests/rust/xhci_dma.rs',
                 '-o', tree / 'tests'], f'mutant-{name}-test-build.txt')
            mlog = run([tree / 'tests', '--test-threads=1', test], f'mutant-{name}-tests.txt', 101)
            if f'{test} ... FAILED' not in mlog:
                raise RuntimeError('mutation control failed for an unrelated reason: ' + name)
            report['mutation_controls'][name] = f'detected by {test} (exit 101)'
        base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
        sources = ['tests/host/usb2c2-dma.c', archive]
        run(base + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] + sources +
            ['-o', out / 'sanitizer'], 'sanitizer-build.txt')
        report['sanitizer'] = run([out / 'sanitizer'], 'sanitizer.txt').strip()
        run(base + ['-O2', '-DUSB2C2_BENCH'] + sources + ['-o', out / 'release'], 'release-build.txt')
        timing = run([out / 'release'], 'release.txt')
        samples = sorted(int(x) for x in re.findall(r'ns_per_alloc_zero_free=(\d+)', timing))
        report['release_ns_per_alloc_zero_free'] = {'samples_sorted': samples, 'median': samples[len(samples) // 2]}
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""USB.2-C3a pure Rust and hosted C/Rust residue-guard tests; fresh output only.

Supply a kernel archive built from this checkout with the normal kernel flags,
and optionally its link map: the kernel must link no Rust core or
compiler_builtins archive member (no panic/formatting machinery). Rust machine
code is not sanitizer-instrumented; the C code and harness are. Mutation
controls edit a temporary copy of the canonical tree.
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
    'reserve-allocated': ('residue.rs', 'Page::Free => match self.m.reserve(frame, role) {',
                          'Page::Free | Page::Allocated => match self.m.reserve(frame, role) {',
                          'already_allocated_frame_is_a_hazard_never_reserved'),
    'inspect-protected': ('residue.rs', '                    self.flag(UNINSPECTED);\n', '',
                          'seabios_residue_in_reserved_memory_is_uninspected_not_safe'),
    'masked-link-pointer': ('residue.rs', 'let Some(target) = self.read(cursor) else { return };',
                            'let Some(target) = self.read(cursor).map(|v| v & !0xf) else { return };',
                            'command_ring_walk_is_bounded'),
    'no-master-off-check': ('lifecycle.rs', '                if !master_off {\n                    return Err(Violation::MissingMasterOff);\n                }\n',
                            '', 'ordering_violations_are_rejected'),
    # USB.2-C3a.3: boot-excluded frames are read in place, never reserved, and the
    # two safety results stay separate.
    'excluded-reserved': ('residue.rs', 'Page::Excluded => self.out.excluded = self.out.excluded.saturating_add(1),',
                          'Page::Excluded => { let _ = self.m.reserve(frame, role); }',
                          'excluded_firmware_residue_is_inspected_in_place_never_reserved'),
    'reuse-ignores-hazard': ('residue.rs', 'self.findings & (HAZARD | EXHAUSTED) == 0', 'self.findings & EXHAUSTED == 0',
                             'reuse_protection_and_inspection_completeness_are_separate'),
    'inspection-ignores-uninspected': ('residue.rs', 'self.findings & !RUNNING == 0',
                                       'self.findings & !(RUNNING | UNINSPECTED) == 0',
                                       'reuse_protection_and_inspection_completeness_are_separate'),
}
COMPILE_FAIL = {
    'plan-cannot-run': ('use vos_xhci::xhci::lifecycle::Step;\nfn main() { let _ = Step::Run; }\n', 'E0599'),
    'memory-has-no-write': ('use vos_xhci::xhci::residue::Memory;\n'
                            'fn f(m: &mut impl Memory) { m.write64(0, 0); }\nfn main() {}\n', 'E0599'),
    'plan-only-from-planner': ('use vos_xhci::xhci::lifecycle::{Plan, Step};\n'
                               'fn main() { let _ = Plan { steps: [Step::Reset; 24], len: 1 }; }\n', 'E0451'),
    'memory-has-no-release': ('use vos_xhci::xhci::residue::Memory;\n'
                              'fn f(m: &mut impl Memory) { m.release(0); }\nfn main() {}\n', 'E0599'),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--kernel-map', type=Path, help='kernel.map of the build that produced the archive')
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
        if args.kernel_map:
            text = args.kernel_map.read_text()
            block = text[text.index('Archive member included'):text.index('Merging object attributes')] \
                if 'Merging object attributes' in text else text[text.index('Archive member included'):]
            members = re.findall(r'\(([^()]+\.o)\)\n', block)
            foreign = [m for m in members if m.startswith(('core-', 'compiler_builtins-'))]
            if foreign:
                raise RuntimeError('kernel links Rust core/compiler_builtins members: ' + ', '.join(foreign[:3]))
            report['kernel_link_rust_members'] = members
        lib = out / 'libvos_xhci.rlib'
        run(rust + ['--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes',
                    '-Cdebug-assertions=yes', CORE / 'lib.rs', '-o', lib], 'core-build.txt')
        run(rust + ['--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_xhci={lib}',
                    'tests/rust/xhci_residue.rs', '-o', out / 'residue-tests'], 'residue-build.txt')
        report['residue_tests'] = re.search(r'test result: .*', run([out / 'residue-tests', '--test-threads=1'],
                                                                     'residue-tests.txt'))[0]
        report['compile_fail_controls'] = {}
        for name, (body, code) in COMPILE_FAIL.items():
            src = out / f'cf-{name}.rs'
            src.write_text(body)
            log = run([args.rustc, '--edition=2024', '--extern', f'vos_xhci={lib}', src, '-o', out / f'cf-{name}'],
                      f'cf-{name}.txt', 1)
            if code not in log:
                raise RuntimeError(f'compile-fail control {name} failed for an unrelated reason')
            report['compile_fail_controls'][name] = f'rejected with {code}'
        report['mutation_controls'] = {}
        for name, (file, old, new, test) in MUTATIONS.items():
            tree = out / ('mutant-' + name)
            shutil.copytree(CORE, tree)
            src = (tree / file).read_text()
            if src.count(old) != 1:
                raise RuntimeError('mutation anchor not unique: ' + name)
            (tree / file).write_text(src.replace(old, new))
            mlib = tree / 'libvos_xhci.rlib'
            run([args.rustc, '--edition=2024', '--crate-name', 'vos_xhci', '--crate-type=rlib', tree / 'lib.rs',
                 '-o', mlib], f'mutant-{name}-build.txt')
            run([args.rustc, '--edition=2024', '--test', '--extern', f'vos_xhci={mlib}', 'tests/rust/xhci_residue.rs',
                 '-o', tree / 'tests'], f'mutant-{name}-test-build.txt')
            mlog = run([tree / 'tests', '--test-threads=1', test], f'mutant-{name}-tests.txt', 101)
            if f'{test} ... FAILED' not in mlog:
                raise RuntimeError('mutation control failed for an unrelated reason: ' + name)
            report['mutation_controls'][name] = f'detected by {test} (exit 101)'
        base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
        run(base + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    'tests/host/usb2c3a-residue.c', archive, '-o', out / 'sanitizer'], 'sanitizer-build.txt')
        report['sanitizer'] = run([out / 'sanitizer'], 'sanitizer.txt').strip()
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

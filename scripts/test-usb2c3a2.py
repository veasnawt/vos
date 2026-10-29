#!/usr/bin/env python3
"""USB.2-C3a.2 pure Rust and hosted C/Rust EFI memory-exclusion tests; fresh output only.

Supply a kernel archive built from this checkout with the normal kernel flags,
and optionally its link map: the kernel must link no Rust core or
compiler_builtins archive member. Rust machine code is not
sanitizer-instrumented; the C code and harness are. Mutation controls edit a
temporary copy of the portable source.
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
CORE = ROOT / 'kernel/rust/boot'
MUTATIONS = {
    'boot-services-allowed': ('efi_memory.rs', 'matches!(self.kind, LOADER_CODE | LOADER_DATA | CONVENTIONAL)',
                              'matches!(self.kind, LOADER_CODE | LOADER_DATA | CONVENTIONAL | 3 | 4)',
                              'ovmf_fixture_excludes_exactly_boot_services_and_firmware_dma_pages'),
    'uncovered-ram-allowed': ('efi_memory.rs', '(ram_here && (bad || !ok), next)', '(ram_here && bad, next)',
                              'overlaps_contradictions_and_attributes_resolve_toward_exclusion'),
    'unchecked-end': ('efi_memory.rs', 'let end = start.checked_add(length).ok_or(Error::Overflow)?;',
                      'let end = start.wrapping_add(length);', 'malformed_efi_maps_are_rejected'),
    'coalesce-drops-range': ('efi_memory.rs', 'Some((ps, _)) if self.emitted + 1 >= self.capacity => {\n'
                             '                self.summary.coalesced = true;\n                Some((ps, e))',
                             'Some((ps, pe)) if self.emitted + 1 >= self.capacity => {\n'
                             '                self.summary.coalesced = true;\n                Some((ps, pe))',
                             'capacity_coalesces_conservatively_and_fails_closed'),
    'missing-map-accepted': ('efi_memory.rs', '0 if e.efi_firmware => Err(Error::Missing),',
                             '0 if e.efi_firmware => Ok(Plan::NotApplicable),', 'bios_and_policy_decisions_fail_closed'),
}
COMPILE_FAIL = {
    'efi-map-only-from-parse': ('use vos_boot::efi_memory::EfiMap;\n'
                                'fn main() { let _ = EfiMap { bytes: &[], size: 48, count: 1 }; }\n', 'E0451'),
    'memory-map-only-from-parse': ('use vos_boot::efi_memory::MemoryMap;\n'
                                   'fn main() { let _ = MemoryMap { bytes: &[], size: 24, count: 1 }; }\n', 'E0451'),
    'no-release-plan': ('use vos_boot::efi_memory::Plan;\nfn main() { let _ = Plan::Release; }\n', 'E0599'),
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
    for p in sorted(CORE.glob('*.rs')) + sorted((ROOT / 'tests/rust/fixtures').glob('*.bin')):
        if p.suffix == '.rs' and re.search(r'\bunsafe\b|\bextern\b|\basm!|\balloc::|\bstd::', p.read_text()):
            raise SystemExit('nonportable/unsafe core source: ' + str(p))
        report['source_sha256'][str(p.relative_to(ROOT))] = hashlib.sha256(p.read_bytes()).hexdigest()

    def run(argv, name, expected=0):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=600,
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
        lib = out / 'libvos_boot.rlib'
        run(rust + ['--crate-name', 'vos_boot', '--crate-type=rlib', '-Coverflow-checks=yes',
                    '-Cdebug-assertions=yes', CORE / 'lib.rs', '-o', lib], 'core-build.txt')
        run(rust + ['--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_boot={lib}',
                    'tests/rust/efi_memory.rs', '-o', out / 'efi-tests'], 'efi-build.txt')
        report['efi_tests'] = re.search(r'test result: .*', run([out / 'efi-tests', '--test-threads=1'],
                                                                'efi-tests.txt'))[0]
        report['compile_fail_controls'] = {}
        for name, (body, code) in COMPILE_FAIL.items():
            src = out / f'cf-{name}.rs'
            src.write_text(body)
            log = run([args.rustc, '--edition=2024', '--extern', f'vos_boot={lib}', src, '-o', out / f'cf-{name}'],
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
            mlib = tree / 'libvos_boot.rlib'
            run([args.rustc, '--edition=2024', '--crate-name', 'vos_boot', '--crate-type=rlib',
                 '--cap-lints=allow', tree / 'lib.rs', '-o', mlib], f'mutant-{name}-build.txt')
            run([args.rustc, '--edition=2024', '--test', '--extern', f'vos_boot={mlib}', 'tests/rust/efi_memory.rs',
                 '-o', tree / 'tests'], f'mutant-{name}-test-build.txt')
            mlog = run([tree / 'tests', '--test-threads=1', test], f'mutant-{name}-tests.txt', 101)
            if f'{test} ... FAILED' not in mlog:
                raise RuntimeError('mutation control failed for an unrelated reason: ' + name)
            report['mutation_controls'][name] = f'detected by {test} (exit 101)'
        base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
        run(base + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-fno-omit-frame-pointer', 'tests/host/usb2c3a2-efi.c', archive, '-o', out / 'sanitizer'],
            'sanitizer-build.txt')
        report['sanitizer'] = run([out / 'sanitizer', 'tests/rust/fixtures'], 'sanitizer.txt').strip()
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

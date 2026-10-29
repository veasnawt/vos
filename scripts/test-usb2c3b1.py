#!/usr/bin/env python3
"""USB.2-C3b.1 containment dry-run checks; fresh output only.

Pure Rust tests of the containment policy and dry-run state machine, mutation
controls (each must be caught by a named test), compile-fail controls, and
static proofs that the build has no PCI-write or register-write path: no new
source names a write primitive, and neither the kernel's Rust archive nor the
xHCI C objects reference a PCI write symbol. The sanitized C scenarios for the
dry-run live in tests/host/usb2c3a-residue.c and run with scripts/test-usb2c3a.py.
Supply a kernel build directory made from this checkout with the normal flags.
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
C = 'containment.rs'
MUTATIONS = {
    'reset-before-bus-master': (C, '[Step::AwaitHalted, Step::DisableMaster, Step::Reset,',
                                '[Step::AwaitHalted, Step::Reset, Step::DisableMaster,',
                                'eligible_idle_uefi_completes_and_issues_nothing'),
    'skip-revalidation-0': (C, '    if now != *first {\n', '    if false {\n',
                            'revalidation_detects_every_change_and_unreadable_pass'),
    'skip-revalidation-1': (C, 'if !command_ok || now != *first {', 'if false {',
                            'revalidation_detects_every_change_and_unreadable_pass'),
    'accept-running': (C, 'set(RUNNING, o.usbcmd & 1 != 0 || o.usbsts & 1 == 0);', 'set(RUNNING, false);',
                       'each_requirement_is_individually_enforced'),
    'accept-slots': (C, 'set(SLOTS, o.findings & DEVICES != 0);', 'set(SLOTS, false);',
                     'each_requirement_is_individually_enforced'),
    'accept-scratchpads': (C, 'set(SCRATCH, o.findings & SCRATCHPADS != 0 || (o.header[2] >> 21) & 0x1f != 0 || '
                              'o.header[2] >> 27 != 0);', 'set(SCRATCH, false);',
                           'each_requirement_is_individually_enforced'),
    'accept-usbsts-bits': (C, 'set(USBSTS, o.usbsts != IDLE_USBSTS);', 'set(USBSTS, o.usbsts & 1 == 0);',
                           'each_requirement_is_individually_enforced'),
    'accept-bios': (C, 'set(NOT_UEFI, !c.uefi);', 'set(NOT_UEFI, false);', 'each_requirement_is_individually_enforced'),
    'ignore-pending-interrupts': (C, '            && self.ip_pending == 0\n', '',
                                  'd1_mask_is_omitted_only_when_interrupts_are_verifiably_disabled'),
    'drop-pcie-busy': (C, 'set(PCIE_BUSY, o.pcie == PCIE_PENDING);', 'set(PCIE_BUSY, false);',
                       'each_requirement_is_individually_enforced'),
    'issued-true': (C, 'pub const ISSUED: bool = false;', 'pub const ISSUED: bool = true;',
                    'eligible_idle_uefi_completes_and_issues_nothing'),
    'w1-wide': (C, 'pub const W1_WIDTH: u8 = 16;', 'pub const W1_WIDTH: u8 = 32;',
                'proposed_writes_have_exact_widths_offsets_masks_and_preserve_reserved_bits'),
    'w1-clears-more': (C, 'let new = first.command & !BUS_MASTER;', 'let new = first.command & !(BUS_MASTER | 2);',
                       'eligible_idle_uefi_completes_and_issues_nothing'),
    'trace-claims-issued': (C, 'R_NOT_ISSUED, W1_WIDTH', 'R_OK, W1_WIDTH',
                            'trace_is_data_only_and_every_write_is_marked_not_issued'),
    'reset-timeout-accepted': (C, 'if r.polls > POLL_BOUND || !r.hcrst_clear || !r.cnr_clear {',
                               'if !r.hcrst_clear || !r.cnr_clear {', 'reset_completion_and_timeout_model'),
    'stuck-bus-master-accepted': (C, 'if after.command & BUS_MASTER != 0 {', 'if false {', 'bus_master_failure_model'),
    'mask-never-needed': ('lifecycle.rs', 'if f.interrupts_disabled { 0 } else { f.interrupter_enables }', '0',
                          'd1_mask_is_omitted_only_when_interrupts_are_verifiably_disabled'),
}
COMPILE_FAIL = {
    'no-executor': ('fn main() { vos_xhci::xhci::containment::execute(); }\n', 'E0425'),
    'source-has-no-write': ('use vos_xhci::xhci::containment::Source;\n'
                            'fn f(s: &mut impl Source) { s.write(4, 3); }\nfn main() {}\n', 'E0599'),
    'rehearsal-cannot-issue': ('use vos_xhci::xhci::containment::Rehearsal;\n'
                               'fn f(r: &Rehearsal) { r.issue(); }\nfn main() {}\n', 'E0599'),
}
# New dry-run sources: no write primitive may be named at all.
WRITE_FREE = ['kernel/rust/usb/xhci/containment.rs', 'arch/x86_64/rust/xhci_contain.rs']
WRITE_PRIMITIVE = re.compile(r'pci_write|\bout[bwl]?\s*\(|\bout(8|16|32)\b|write_volatile|\bwrite32\b|\bwrite64\b|asm!')
PCI_WRITE_SYMBOLS = re.compile(r'\bpci_write(8|16|32)\b')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel-build', type=Path, required=True, help='build directory of this checkout')
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    kb = args.kernel_build.resolve()
    archive, kmap = kb / 'rust/libvos_kernel.a', kb / 'kernel.map'
    report = {'commands': [], 'hardware': False, 'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'source_sha256': {}}
    for p in sorted(CORE.glob('*.rs')):
        if re.search(r'\bunsafe\b|\bextern\b|\basm!|\balloc::|\bstd::', p.read_text()):
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
        # Static: no write path exists in this build.
        named = {f: [m.group(0) for m in WRITE_PRIMITIVE.finditer((ROOT / f).read_text())] for f in WRITE_FREE}
        if any(named.values()):
            raise RuntimeError('a dry-run source names a write primitive: ' + json.dumps(named))
        refs = {}
        for f in (archive, kb / 'xhci_qualify.o', kb / 'efi_memory.o'):
            undefined = subprocess.run(['nm', '-u', f], capture_output=True, text=True, check=True).stdout
            refs[f.name] = sorted(set(m.group(0) for m in PCI_WRITE_SYMBOLS.finditer(undefined)))
        if any(refs.values()):
            raise RuntimeError('PCI write symbol referenced: ' + json.dumps(refs))
        report['no_write_path'] = {'sources_without_write_primitives': WRITE_FREE,
                                   'pci_write_references': refs}
        text = kmap.read_text()
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
                    'tests/rust/xhci_containment.rs', '-o', out / 'containment-tests'], 'containment-build.txt')
        report['containment_tests'] = re.search(r'test result: .*', run([out / 'containment-tests', '--test-threads=1'],
                                                                        'containment-tests.txt'))[0]
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
            run([args.rustc, '--edition=2024', '--crate-name', 'vos_xhci', '--crate-type=rlib', '--cap-lints=allow',
                 tree / 'lib.rs', '-o', mlib], f'mutant-{name}-build.txt')
            run([args.rustc, '--edition=2024', '--test', '--extern', f'vos_xhci={mlib}', 'tests/rust/xhci_containment.rs',
                 '-o', tree / 'tests'], f'mutant-{name}-test-build.txt')
            mlog = run([tree / 'tests', '--test-threads=1', test], f'mutant-{name}-tests.txt', 101)
            if f'{test} ... FAILED' not in mlog:
                raise RuntimeError('mutation control failed for an unrelated reason: ' + name)
            report['mutation_controls'][name] = f'detected by {test} (exit 101)'
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

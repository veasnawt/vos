#!/usr/bin/env python3
"""USB.2-C3b.2 hosted checks (fresh output only): the QEMU-only controlled-containment executor.

  * pure Rust executor tests (model device, independent oracle, 20,000 randomized fault combinations);
  * compile-fail controls and a production core that has no executor at all;
  * mutation controls on the Rust core, the platform Rust and the C write adapters, each caught by a named test;
  * the sanitized C harness (real PMM/devmap/qualification/residue C, the real adapters and the real Rust archive);
  * the static write-path proof: exactly two reachable write effects (W1, W2) in the test image, none in production;
  * production isolation: negative `make` gates, a fully smuggled executor stopped by the binary check, binary
    inspection of production kernels and ISO payloads.
Supply a production build and a QEMU test-image build of this checkout (see the C3b.2 report for the commands).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'kernel/rust/usb/xhci'
ACK = 'VOS_CONTAIN_TEST_ACK=QEMU-ONLY-EXPERIMENTAL'
CQ = 'xhci_qualify.c'
CX = 'xhci_contain.rs'
# (name, file under CORE or PLATFORM, old, new, test that must fail)
RUST_MUTATIONS = {
    'skip-w1-readback-bme': ('containment.rs', '    if q.command & BUS_MASTER != 0 {\n        return State::BusMasterStuck;', '    if false {\n        return State::BusMasterStuck;',
                             'w1_ignored_by_the_device_is_final_and_blocks_the_reset'),
    'skip-pci-changed': ('containment.rs', '    if q.command != off || q.status != first.status {', '    if false {',
                         'every_single_fault_matches_the_oracle_and_invariants'),
    'skip-controller-error': ('containment.rs', '    if q.usbsts != IDLE_USBSTS || q.usbcmd != first.usbcmd {', '    if false {',
                              'every_single_fault_matches_the_oracle_and_invariants'),
    'skip-post-w1-observation': ('containment.rs', '    if !e.observe(&mut now) || now.command != off {\n        return State::ContainedBmeOnly;\n    }\n    now.command = first.command;\n    if now != *first {',
                                 '    if !e.observe(&mut now) || now.command != off {\n        return State::ContainedBmeOnly;\n    }\n    now.command = first.command;\n    if false {',
                                 'every_single_fault_matches_the_oracle_and_invariants'),
    'skip-pre-w2-reread': ('containment.rs', '    if !e.quick(&mut q, true) || q != (Quick { command: off, ..idle }) {', '    if !e.quick(&mut q, true) {',
                           'every_single_fault_matches_the_oracle_and_invariants'),
    'skip-pre-w1-reread': ('containment.rs', '|| !e.quick(&mut q, true) || q != idle {', '|| !e.quick(&mut q, true) {',
                           'every_single_fault_matches_the_oracle_and_invariants'),
    'skip-pre-w1-observation': ('containment.rs', 'if !e.observe(&mut now) || now != *first || !e.quick', 'if !e.observe(&mut now) || !e.quick',
                                'every_single_fault_matches_the_oracle_and_invariants'),
    'unbounded-poll': ('containment.rs', '        if x.polls == POLL_BOUND || !e.quick(&mut q, false) {', '        if !e.quick(&mut q, false) {',
                       'reset_timeout_is_bounded_and_exact'),
    'accept-cnr-stuck': ('containment.rs', 'if q.usbcmd & HCRST == 0 && q.usbsts & CNR == 0 {', 'if q.usbcmd & HCRST == 0 {',
                         'reset_timeout_is_bounded_and_exact'),
    'accept-unverified': ('containment.rs', '|| !reset_verified(first, &x.after) {', '{',
                          'verification_failures_end_unverified_with_both_writes_made'),
    'verify-drops-bme': ('containment.rs', ' && a.command == first.command & !BUS_MASTER', '',
                         'reset_verification_checks_every_field_individually'),
    'verify-drops-crcr': ('containment.rs', ' && a.crcr == 0 && a.dcbaap == 0', ' && a.dcbaap == 0',
                          'reset_verification_checks_every_field_individually'),
    'verify-drops-interrupters': ('containment.rs', ' && !a.ports_active && a.interrupters_clear', ' && !a.ports_active',
                                  'reset_verification_checks_every_field_individually'),
    'verify-drops-ports': ('containment.rs', ' && !a.ports_active && a.interrupters_clear', ' && a.interrupters_clear',
                           'reset_verification_checks_every_field_individually'),
    'w1-clears-more': ('containment.rs', '    let off = first.command & !BUS_MASTER;\n    x.w1[0]', '    let off = first.command & !(BUS_MASTER | 1);\n    x.w1[0]',
                       'eligible_run_issues_exactly_w1_then_w2'),
    'execute-without-flag': ('containment.rs', 'if rehearsal.state != State::DryRunComplete || !c.execute {', 'if rehearsal.state != State::DryRunComplete {',
                             'execution_needs_the_execute_flag_even_when_eligible'),
    'writes-not-counted': ('containment.rs', '    x.writes = 1;\n    // From here', '    // From here', 'every_single_fault_matches_the_oracle_and_invariants'),
    'skip-subsystem-refusal': ('containment.rs', 'set(SUBSYSTEM, c.execute && o.subsystem != SUBSYSTEM_ID);', 'set(SUBSYSTEM, false);',
                               'environment_and_subsystem_refuse_only_when_executing'),
    'skip-environment-refusal': ('containment.rs', 'set(ENVIRONMENT, c.execute && c.environment != 0);', 'set(ENVIRONMENT, false);',
                                 'environment_and_subsystem_refuse_only_when_executing'),
    'ignore-phase-failure': ('containment.rs', '    if !e.phase(2 * POLL_BOUND) {\n        return State::ResetStuck;\n    }', '    let _ = e.phase(2 * POLL_BOUND);',
                             'every_single_fault_matches_the_oracle_and_invariants'),
    'bus-master-stuck-not-detected': ('containment.rs', '    if q.command & BUS_MASTER != 0 {\n        return State::BusMasterStuck;\n    }', '    if q.command & BUS_MASTER != 0 {\n        // continue to the reset\n    }',
                          'w1_ignored_by_the_device_is_final_and_blocks_the_reset'),
}
# C adapter and boot mutations: (old, new, scenario that must fail)
C_MUTATIONS = {
    'w1-accepts-clear-bme': ('!(old & 4) ||', '', 'W1 adapter'),
    'w1-any-value': (' || value != (old & ~4u)', '', 'W1 adapter'),
    'w1-more-than-once': ('!exe.armed || exe.w1 || !qual.active', '!exe.armed || !qual.active', 'W1 adapter'),
    'w1-no-device-compare': (" ||\n        pci_read16(qual.fn, 4) != old)\n        return DEVMAP_ESTATE;\n    exe.w1 = 1;", ")\n        return DEVMAP_ESTATE;\n    exe.w1 = 1;", 'W1 adapter'),
    'w1-ignores-context': ('!exe.armed || exe.w1 || !qual.active || !qualify_context() ||', '!exe.armed || exe.w1 || !qual.active ||', 'W1 adapter'),
    'w2-without-w1': ('!exe.armed || !exe.w1 || exe.w2', '!exe.armed || exe.w2', 'W2 adapter'),
    'w2-bme-on': ('(pci_read16(qual.fn, 4) & 4) || value', 'value', 'W2 adapter'),
    'w2-any-value': ('value != (old | 2u) || offset & 3', 'offset & 3', 'W2 adapter'),
    'w2-caplength': (' ||\n        offset != (XHCI_QUALIFY_READ32(va, 0) & 0xff) || XHCI_QUALIFY_READ32(va + offset, 0) != old)', ' ||\n        XHCI_QUALIFY_READ32(va + offset, 0) != old)', 'W2 adapter'),
    'w2-stale-old': (' || XHCI_QUALIFY_READ32(va + offset, 0) != old)\n        return DEVMAP_ESTATE;\n    exe.w2 = 1;', ')\n        return DEVMAP_ESTATE;\n    exe.w2 = 1;', 'USBCMD changed'),
    'w2-more-than-once': ('!exe.armed || !exe.w1 || exe.w2', '!exe.armed || !exe.w1', 'W2 adapter'),
    'conflict-contain-dropped': ('"vos.usb2-contain=1", "vos.usb2-residue=1"', '"vos.usb2-residue=1"', 'conflicting option'),
    'conflict-probe-dropped': (', "vos.usb_probe=1" };', ' };', 'conflicting option'),
    'tcg-check-dropped': ('if (b == 0x54474354 && c == 0x43544743 && d == 0x47435447) bad &= ~1u;', 'bad &= ~1u;', 'KVM signature'),
    'hypervisor-bit-dropped': ('    if (c >> 31) {\n        cpuid(0x40000000', '    {\n        cpuid(0x40000000', 'no hypervisor bit'),
    'oem-check-dropped': ('!memcmp(p + 8 + 9, "BOCHS ", 6))\n                bad &= ~2u;', '1)\n                bad &= ~2u;', 'ACPI OEM ID'),
    'hostbridge-check-dropped': ('if (pci_read32(host, 0) == 0x29c08086 && pci_read32(host, 0x2c) == 0x11001af4) bad &= ~4u;', '(void)host; bad &= ~4u;', 'host bridge'),
    'freeze-not-held': ('residue.frozen = 1; exe.armed = 1;', 'exe.armed = 1;', 'eligible'),
    'armed-left-on': ('exe.armed = 0; qual.active = 0; residue.session = 0; residue.frozen = 0;', 'qual.active = 0; residue.session = 0; residue.frozen = 0;', 'eligible'),
    'once-per-boot-dropped': ('if (exe.ran++) return 0;', 'exe.ran++;', 'eligible'),
}
# Platform Rust mutations, built into a kernel archive and run through the sanitized harness.
PLATFORM_MUTATIONS = {
    'hook-claims-bme-always': ('Ok(if e.writes > 0 && e.w1[2] != 0 && e.w1[2] & BUS_MASTER == 0 { BUS_MASTER } else { 0 })',
                               'Ok(if e.writes > 0 { BUS_MASTER } else { 0 })', 'W1 ignored'),
    'ports-mask-includes-change': ('a.ports_active = active & 0x1b != 0;', 'a.ports_active = active & 0x00fe_001b != 0;', 'eligible'),
    'ports-never-checked-before-writes': ('for p in 0..u32::from(self.i.ports) * u32::from(ports) {', 'let _ = ports;\n            for p in 0..0u32 {', 'port reads'),
    'interrupters-unchecked': ('a.interrupters_clear = any == 0;', 'let _ = any;\n            a.interrupters_clear = true;', 'event ring pointer'),
    'crcr-unchecked-read': ('a.crcr = self.rd64(op + 0x18)?;', 'a.crcr = 0; let _ = self.rd64(op + 0x18)?;', 'command ring pointer'),
    'imod-required-zero': ('for off in [0, 8, 0x10, 0x14, 0x18, 0x1c] {', 'for off in [0, 4, 8, 0x10, 0x14, 0x18, 0x1c] {', 'IMOD'),
}


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


class Run:
    def __init__(self, out, report):
        self.out, self.report = out, report

    def __call__(self, argv, name, expected=0, cwd=ROOT, env=None):
        argv = [str(a) for a in argv]
        self.report['commands'].append(argv)
        with (self.out / name).open('x') as f:
            p = subprocess.run(argv, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, timeout=900,
                               env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(env or {})))
        if p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (self.out / name).read_text()


def calls_in(text):
    """Defined functions of an objdump -d listing: {name: set(direct call targets)}."""
    graph, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            cur = m.group(1)
            graph[cur] = set()
            continue
        m = re.search(r'\b(call|jmp)\s+[0-9a-f]+ <([^>+]+)(?:\+0x[0-9a-f]+)?>', line)
        if m and cur:
            graph[cur].add(m.group(2))
    return graph


def reachable(graph, roots):
    seen, todo = set(), list(roots)
    while todo:
        f = todo.pop()
        if f in seen or f not in graph:
            continue
        seen.add(f)
        todo.extend(graph[f])
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--production-build', type=Path, required=True, help='make build directory (no VOS_CONTAIN_TEST)')
    ap.add_argument('--test-build', type=Path, required=True, help='make build directory made with VOS_CONTAIN_TEST=1')
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--iso', type=Path, action='append', default=[], help='production ISO(s) to inspect')
    ap.add_argument('--skip-mutations', action='store_true', help='debugging only; the final evidence runs them all')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    pb, tb = args.production_build.resolve(), args.test_build.resolve()
    report = {'commands': [], 'hardware': False, 'physical_hardware': False}
    run = Run(out, report)
    rust = [args.rustc, '--edition=2024', '-Dwarnings']
    try:
        for p in sorted(CORE.glob('*.rs')):
            if re.search(r'\bunsafe\b|\bextern\b|\basm!|\balloc::|\bstd::', p.read_text()):
                raise RuntimeError('nonportable/unsafe core source: ' + str(p))
        report['source_sha256'] = {str(p.relative_to(ROOT)): sha(p) for p in sorted(CORE.glob('*.rs'))}
        report['source_sha256'].update({f: sha(ROOT / f) for f in (
            'arch/x86_64/xhci_qualify.c', 'arch/x86_64/xhci_qualify.h', 'arch/x86_64/rust/xhci_contain.rs',
            'arch/x86_64/rust/xhci_qualify.rs', 'arch/x86_64/arch.mk', 'Makefile', 'scripts/check-contain-image.py',
            'tests/rust/xhci_contain_execute.rs', 'tests/host/usb2c3b2-execute.c')})
        test_archive, prod_archive = tb / 'rust/libvos_kernel.a', pb / 'rust/libvos_kernel.a'
        report['archives'] = {'production': sha(prod_archive), 'test': sha(test_archive),
                              'production_vos_bin': sha(pb / 'vos.bin'), 'test_vos_bin': sha(tb / 'vos.bin')}

        # ---- 1. Production isolation, from the binaries ------------------------------------------------------
        check = [ROOT / 'scripts/check-contain-image.py']
        run(['python3', '-I', '-B', check[0], '--expect', 'production', pb / 'vos.bin'], 'isolation-production-kernel.txt')
        run(['python3', '-I', '-B', check[0], '--expect', 'test', tb / 'vos.bin'], 'isolation-test-kernel.txt')
        run(['python3', '-I', '-B', check[0], '--expect', 'production', tb / 'vos.bin'], 'isolation-test-as-production.txt', 1)
        run(['python3', '-I', '-B', check[0], '--expect', 'test', pb / 'vos.bin'], 'isolation-production-as-test.txt', 1)
        syms = {}
        for label, kb in (('production', pb), ('test', tb)):
            names = {l.split()[-1] for l in subprocess.check_output(['nm', kb / 'vos.bin'], text=True).splitlines() if len(l.split()) == 3}
            syms[label] = sorted(n for n in names if re.search(r'xhci_contain_(w1|w2|execute)|vos_xhci_contain_execute|vos_contain_test_image', n))
            und = subprocess.check_output(['nm', '-u', kb / 'xhci_qualify.o'], text=True)
            syms[label + '_xhci_qualify_o_pci_write_refs'] = sorted(set(re.findall(r'\bpci_write(?:8|16|32)\b', und)))
            data = (kb / 'vos.bin').read_bytes()
            syms[label + '_option_string'] = b'vos.usb2-contain-execute=1' in data
            syms[label + '_marker'] = b'VOS-CONTAIN-TEST-IMAGE' in data
        if syms['production'] or syms['production_xhci_qualify_o_pci_write_refs'] or syms['production_option_string'] or syms['production_marker']:
            raise RuntimeError('production kernel carries the executor: ' + json.dumps(syms))
        if syms['test_xhci_qualify_o_pci_write_refs'] != ['pci_write16'] or len(syms['test']) < 4:
            raise RuntimeError('test kernel write references unexpected: ' + json.dumps(syms))
        report['isolation_symbols'] = syms
        # ISO payloads: the kernel inside, and the raw bytes of the whole image.
        isos = {}
        for iso in args.iso:
            run(['python3', '-I', '-B', check[0], '--expect', 'production', '--iso', iso], f'iso-{iso.stem}-check.txt')
            isos[str(iso)] = {'iso_sha256': sha(iso), 'production': True}
        report['iso_payloads'] = isos

        # ---- 2. The negative build gate (make parses; nothing is built) --------------------------------------
        bt = 'build/usb2c3b2-gate-probe-contain-test'
        gate = {
            'no ACK': (['VOS_CONTAIN_TEST=1', f'BUILD={bt}'], {}, 'also requires VOS_CONTAIN_TEST_ACK'),
            'wrong ACK': (['VOS_CONTAIN_TEST=1', 'VOS_CONTAIN_TEST_ACK=yes', f'BUILD={bt}'], {}, 'must be exactly QEMU-ONLY-EXPERIMENTAL'),
            'BUILD name': (['VOS_CONTAIN_TEST=1', ACK, 'BUILD=build/usb2c3b2-gate-probe'], {}, 'requires BUILD='),
            'iso goal': (['VOS_CONTAIN_TEST=1', ACK, f'BUILD={bt}', 'iso'], {}, 'refused goals: iso'),
            'uefi-iso goal': (['VOS_CONTAIN_TEST=1', ACK, f'BUILD={bt}', 'uefi-iso'], {}, 'refused goals: uefi-iso'),
            'run goal': (['VOS_CONTAIN_TEST=1', ACK, f'BUILD={bt}', 'run'], {}, 'refused goals: run'),
            'input-iso goal': (['VOS_CONTAIN_TEST=1', ACK, f'BUILD={bt}', 'input-iso'], {}, 'refused goals: input-iso'),
            'VOS_TEST combined': (['VOS_CONTAIN_TEST=1', ACK, 'VOS_TEST=1', f'BUILD={bt}'], {}, 'cannot be combined'),
            'environment variable': ([f'BUILD={bt}'], {'VOS_CONTAIN_TEST': '1'}, 'must be given on the make command line'),
            'environment + command-line ACK': ([ACK, f'BUILD={bt}'], {'VOS_CONTAIN_TEST': '1'}, 'must be given on the make command line'),
            'environment ACK only': ([], {'VOS_CONTAIN_TEST_ACK': 'QEMU-ONLY-EXPERIMENTAL'}, 'without VOS_CONTAIN_TEST=1'),
            'ACK alone': ([ACK], {}, 'without VOS_CONTAIN_TEST=1'),
            'value yes': (['VOS_CONTAIN_TEST=yes', ACK, f'BUILD={bt}'], {}, 'must be 1 or unset'),
        }
        gate_report = {}
        for name, (extra, env, text) in gate.items():
            env = {k: v for k, v in os.environ.items() if k not in ('VOS_CONTAIN_TEST', 'VOS_CONTAIN_TEST_ACK')} | env
            p = subprocess.run(['make', 'ARCH=x86_64', '-n', 'verify', *extra], cwd=ROOT, capture_output=True, text=True, env=env)
            if p.returncode == 0 or text not in p.stderr:
                raise RuntimeError(f'build gate "{name}" was not refused for the right reason: rc={p.returncode} {p.stderr[-200:]}')
            gate_report[name] = text
        ok = subprocess.run(['make', 'ARCH=x86_64', '-n', 'verify', 'VOS_CONTAIN_TEST=1', ACK, f'BUILD={bt}'], cwd=ROOT, capture_output=True, text=True)
        if ok.returncode != 0:
            raise RuntimeError('the allowed test-image goal was refused')
        report['build_gate_refusals'] = gate_report
        if (ROOT / bt).exists():
            raise RuntimeError('a refused or dry build created its directory')

        # ---- 3. Rust core with and without the executor ------------------------------------------------------
        rlib_p, rlib_t = out / 'libvos_xhci_production.rlib', out / 'libvos_xhci_test.rlib'
        run(rust + ['--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cdebug-assertions=yes',
                    CORE / 'lib.rs', '-o', rlib_p], 'core-production-build.txt')
        run(rust + ['--cfg', 'vos_contain_test', '--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes',
                    '-Cdebug-assertions=yes', CORE / 'lib.rs', '-o', rlib_t], 'core-test-build.txt')
        run(rust + ['--cfg', 'vos_contain_test', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_xhci={rlib_t}',
                    'tests/rust/xhci_contain_execute.rs', '-o', out / 'execute-tests'], 'execute-build.txt')
        log = run([out / 'execute-tests', '--test-threads=1'], 'execute-tests.txt')
        report['executor_tests'] = re.search(r'test result: .*', log)[0]
        # The C3b.1 suite is unchanged and still runs against the production core.
        run(rust + ['--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_xhci={rlib_p}',
                    'tests/rust/xhci_containment.rs', '-o', out / 'containment-tests'], 'containment-build.txt')
        report['c3b1_containment_tests'] = re.search(r'test result: .*', run([out / 'containment-tests', '--test-threads=1'], 'containment-tests.txt'))[0]
        run(rust + ['--test', '--extern', f'vos_xhci={rlib_p}', 'tests/rust/xhci_contain_execute.rs', '-o', out / 'execute-in-production'],
            'execute-in-production.txt', 1)
        if 'E0425' not in (out / 'execute-in-production.txt').read_text() and 'E0432' not in (out / 'execute-in-production.txt').read_text() \
                and 'E0433' not in (out / 'execute-in-production.txt').read_text():
            raise RuntimeError('the executor tests did not fail to compile against the production core for the right reason')
        report['production_core_has_no_executor'] = 'the executor tests do not compile against the production core'
        controls = {
            'no-executor-in-production': (rlib_p, 'fn main() { let mut f = vos_xhci::xhci::containment::Observed::default(); '
                                                  'let _ = vos_xhci::xhci::containment::execute; }\n', 'E0425'),
            'effects-has-no-bme-setter': (rlib_t, 'use vos_xhci::xhci::containment::Effects;\nfn f(e: &mut dyn Effects) { e.enable_bus_master(); }\nfn main() {}\n', 'E0599'),
            'effects-has-no-pci-write': (rlib_t, 'use vos_xhci::xhci::containment::Effects;\nfn f(e: &mut dyn Effects) { e.write_pci(0x10, 1); }\nfn main() {}\n', 'E0599'),
            'effects-has-no-register-write': (rlib_t, 'use vos_xhci::xhci::containment::Effects;\nfn f(e: &mut dyn Effects) { e.write_register(0x44, 1); }\nfn main() {}\n', 'E0599'),
            'source-has-no-write': (rlib_t, 'use vos_xhci::xhci::containment::Source;\nfn f(s: &mut dyn Source) { s.write(4, 3); }\nfn main() {}\n', 'E0599'),
            'rehearsal-cannot-issue': (rlib_t, 'use vos_xhci::xhci::containment::Rehearsal;\nfn f(r: &Rehearsal) { r.issue(); }\nfn main() {}\n', 'E0599'),
        }
        report['compile_fail_controls'] = {}
        for name, (lib, body, code) in controls.items():
            src = out / f'cf-{name}.rs'
            src.write_text(body)
            cfg = ['--cfg', 'vos_contain_test'] if lib == rlib_t else []
            log = run([args.rustc, '--edition=2024', *cfg, '--extern', f'vos_xhci={lib}', src, '-o', out / f'cf-{name}'], f'cf-{name}.txt', 1)
            if code not in log:
                raise RuntimeError(f'compile-fail control {name} failed for an unrelated reason')
            report['compile_fail_controls'][name] = f'rejected with {code}'

        # ---- 4. Static proof of exactly two write effects ---------------------------------------------------
        qc = (ROOT / 'arch/x86_64/' / CQ).read_text()
        test_block = qc[qc.index('#ifdef VOS_CONTAIN_TEST\n/* ---- USB.2-C3b.2'):]
        before_block = qc[:qc.index('#ifdef VOS_CONTAIN_TEST\n/* ---- USB.2-C3b.2')]
        code_only = lambda s: re.sub(r'/\*.*?\*/|//[^\n]*', '', s, flags=re.S)
        t, b = code_only(test_block), code_only(before_block)
        counts = {'pci_write16_calls_in_test_block': len(re.findall(r'\bpci_write16\s*\(', t)),
                  'pci_write8_or_32_calls': len(re.findall(r'\bpci_write(?:8|32)\s*\(', qc)),
                  'register_stores_in_test_block': len(re.findall(r'\bXHCI_CONTAIN_WRITE32\s*\(', t)) - len(re.findall(r'#define\s+XHCI_CONTAIN_WRITE32', t)),
                  'port_writes': len(re.findall(r'\bout[bwl]?\s*\(|\bout(?:8|16|32)\s*\(|\bio_out', code_only(qc))),
                  'pci_write_in_non_test_code': len(re.findall(r'\bpci_write\w*\s*\(', b))}
        if counts != {'pci_write16_calls_in_test_block': 1, 'pci_write8_or_32_calls': 0, 'register_stores_in_test_block': 1,
                      'port_writes': 0, 'pci_write_in_non_test_code': 0}:
            raise RuntimeError('unexpected write sites in xhci_qualify.c: ' + json.dumps(counts))
        rs = (ROOT / 'arch/x86_64/rust' / CX).read_text()
        core = (CORE / 'containment.rs').read_text()
        effects = re.search(r'pub trait Effects: Source \{(.*?)\n\}', core, re.S)[1]
        methods = re.findall(r'fn (\w+)', effects)
        counts['effects_methods'] = methods
        if sorted(m for m in methods if m.startswith('write')) != ['write_command', 'write_usbcmd']:
            raise RuntimeError('Effects exposes unexpected write methods: ' + json.dumps(methods))
        ffi = re.findall(r'fn (xhci_contain_w[12])\(', rs)
        calls = {n: len(re.findall(rf'\b{n}\(', rs)) - 1 for n in ('xhci_contain_w1', 'xhci_contain_w2')}
        if sorted(ffi) != ['xhci_contain_w1', 'xhci_contain_w2'] or calls != {'xhci_contain_w1': 1, 'xhci_contain_w2': 1}:
            raise RuntimeError('platform Rust adapter references unexpected: ' + json.dumps([ffi, calls]))
        counts['rust_adapter_call_sites'] = calls
        for f in ('kernel/rust/usb/xhci/containment.rs', 'arch/x86_64/rust/xhci_contain.rs'):
            hit = re.findall(r'pci_write|\bwrite_volatile|\bwrite32\b|\bwrite64\b|asm!|\bout[bwl]?\s*\(', code_only((ROOT / f).read_text()))
            if hit:
                raise RuntimeError(f'{f} names a write primitive: {hit}')
        # Reachability in the linked test kernel: every path to a PCI write, and to the register store, from the executor.
        dis = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', tb / 'vos.bin'], text=True)
        graph = calls_in(dis)
        roots = [n for n in graph if n == 'xhci_contain_execute_boot']
        # Calls through trait objects are indirect; include every function of the executor's modules as roots.
        roots += [n for n in graph if re.search(r'xhci_contain|containment|xhci_qualify|xhci_residue', n)]
        reach = reachable(graph, roots)
        writers = sorted(n for n in reach if re.fullmatch(r'pci_write(?:8|16|32)', n))
        callers = sorted(n for n, cs in graph.items() if n in reach and 'pci_write16' in cs)
        other_callers = sorted(n for n, cs in graph.items() if n in reach and cs & {'pci_write8', 'pci_write32'})
        counts['reachable_pci_write_functions'] = writers
        counts['callers_of_pci_write16_in_closure'] = callers
        counts['callers_of_pci_write8_or_32_in_closure'] = other_callers
        counts['closure_functions'] = len(reach)
        if writers != ['pci_write16'] or callers != ['xhci_contain_w1'] or other_callers:
            raise RuntimeError('unexpected reachable PCI write paths: ' + json.dumps(counts))
        w2_objdump = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', '--disassemble=xhci_contain_w2', tb / 'xhci_qualify.o'], text=True)
        counts['w2_function_instructions'] = len([l for l in w2_objdump.splitlines() if re.match(r'\s+[0-9a-f]+:', l)])
        report['write_path_proof'] = counts

        # ---- 5. Sanitized C harness --------------------------------------------------------------------------
        base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-fno-pie', '-no-pie', '-Iarch/x86_64', '-DVOS_CONTAIN_TEST', '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        run(base + ['tests/host/usb2c3b2-execute.c', test_archive, '-o', out / 'sanitizer'], 'sanitizer-build.txt')
        san = run([out / 'sanitizer'], 'sanitizer.txt')
        report['sanitizer'] = {'summary': san.strip().splitlines()[-1],
                               'scenarios': len(re.findall(r'checks PASS', san)),
                               'checks': sum(int(x) for x in re.findall(r'(\d+) checks PASS', san))}

        # ---- 6. Mutation controls ---------------------------------------------------------------------------
        report['mutation_controls'] = {}
        if not args.skip_mutations:
            for name, (file, old, new, test) in RUST_MUTATIONS.items():
                tree = out / ('mutant-' + name)
                shutil.copytree(CORE, tree)
                src = (tree / file).read_text()
                if src.count(old) != 1:
                    raise RuntimeError('mutation anchor not unique: ' + name)
                (tree / file).write_text(src.replace(old, new))
                mlib = tree / 'libvos_xhci.rlib'
                run([args.rustc, '--edition=2024', '--cfg', 'vos_contain_test', '--crate-name', 'vos_xhci', '--crate-type=rlib',
                     '--cap-lints=allow', tree / 'lib.rs', '-o', mlib], f'mutant-{name}-build.txt')
                run([args.rustc, '--edition=2024', '--cfg', 'vos_contain_test', '--test', '--extern', f'vos_xhci={mlib}',
                     'tests/rust/xhci_contain_execute.rs', '-o', tree / 'tests'], f'mutant-{name}-test-build.txt')
                mlog = run([tree / 'tests', '--test-threads=1', test], f'mutant-{name}-tests.txt', 101)
                if f'{test} ... FAILED' not in mlog:
                    raise RuntimeError('mutation control failed for an unrelated reason: ' + name)
                report['mutation_controls'][name] = f'detected by {test} (exit 101)'
                shutil.rmtree(tree)
            tmp = out / 'c-mutant'
            for name, (old, new, scenario) in C_MUTATIONS.items():
                if tmp.exists():
                    shutil.rmtree(tmp)
                (tmp / 'tests/host').mkdir(parents=True)
                shutil.copytree(ROOT / 'arch/x86_64', tmp / 'arch/x86_64', ignore=shutil.ignore_patterns('*.o', 'rust'))
                shutil.copy(ROOT / 'tests/host/usb2c3b2-execute.c', tmp / 'tests/host')
                path = tmp / 'arch/x86_64' / CQ
                src = path.read_text()
                if src.count(old) != 1:
                    raise RuntimeError('C mutation anchor not unique: ' + name)
                path.write_text(src.replace(old, new))
                run(base + ['tests/host/usb2c3b2-execute.c', test_archive, '-o', tmp / 'sanitizer'], f'cmutant-{name}-build.txt', cwd=tmp)
                mlog = run([tmp / 'sanitizer'], f'cmutant-{name}.txt', 1, cwd=tmp)
                m = re.search(r'scenario (.*) FAILED', mlog)
                if not m:
                    raise RuntimeError(f'C mutation {name} failed for an unrelated reason: {mlog[-300:]}')
                report['mutation_controls'][name] = f'detected: scenario "{m[1]}" failed'
                shutil.rmtree(tmp)
            inputs = (tb / 'rust/inputs').read_text().splitlines()[1]
            flags = [f for f in shlex.split(inputs) if not f.startswith('--remap-path-prefix=')]
            flags += [f'--remap-path-prefix={ROOT}=.', '--emit=link']
            for name, (old, new, scenario) in PLATFORM_MUTATIONS.items():
                tree = out / ('pmutant-' + name)
                (tree / 'arch/x86_64').mkdir(parents=True)
                shutil.copytree(ROOT / 'arch/x86_64/rust', tree / 'arch/x86_64/rust')
                shutil.copytree(ROOT / 'kernel/rust', tree / 'kernel/rust')
                path = tree / 'arch/x86_64/rust' / CX
                src = path.read_text()
                if src.count(old) != 1:
                    raise RuntimeError('platform mutation anchor not unique: ' + name)
                path.write_text(src.replace(old, new))
                archive = tree / 'libvos_kernel.a'
                run([args.rustc, *flags, '-o', archive, tree / 'arch/x86_64/rust/kernel_probe.rs'], f'pmutant-{name}-build.txt', cwd=tree)
                run(base + ['tests/host/usb2c3b2-execute.c', archive, '-o', tree / 'sanitizer'], f'pmutant-{name}-san-build.txt')
                mlog = run([tree / 'sanitizer'], f'pmutant-{name}.txt', 1)
                m = re.search(r'scenario (.*) FAILED', mlog)
                if not m:
                    raise RuntimeError(f'platform mutation {name} failed for an unrelated reason: {mlog[-300:]}')
                report['mutation_controls'][name] = f'detected: scenario "{m[1]}" failed'
                shutil.rmtree(tree)

        # ---- 7. A fully smuggled executor in an ordinary make: the binary check stops it --------------------
        smug = out / 'smuggle-probe'
        smug_rel = smug.relative_to(ROOT)
        env = dict(os.environ, KERNEL_RUST_EXTRA_FLAGS='--cfg vos_contain_test', TEST_DEFS='-DVOS_CONTAIN_TEST')
        for k in ('VOS_CONTAIN_TEST', 'VOS_CONTAIN_TEST_ACK'):
            env.pop(k, None)
        p = subprocess.run(['make', 'ARCH=x86_64', f'BUILD={smug_rel}', f'RUSTC={args.rustc}', 'verify'], cwd=ROOT, capture_output=True,
                           text=True, env=env, timeout=900)
        (out / 'smuggle-build.txt').write_text(p.stdout[-6000:] + p.stderr[-6000:])
        if p.returncode == 0 or 'NOT a production image' not in p.stderr or (smug / 'vos.bin').exists():
            raise RuntimeError('a smuggled executor survived an ordinary production make')
        report['smuggled_executor'] = 'linked, then rejected by check-contain-image.py; vos.bin removed'
        shutil.rmtree(smug)   # this script's own temporary build; its log is kept (smuggle-build.txt)
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""USB.3-A hosted verification of the EHCI qualification and handoff model.

Everything here is host-only: no QEMU, no kernel build, no device, PCI, MMIO or DMA access. The model lives in
kernel/rust/usb/ehci_init/ and is not referenced by any kernel, ISO or Makefile input.

Gates, in order (the first failure stops the run and keeps its logs):
  1. static isolation: the model is #![no_std] and #![forbid(unsafe_code)], has no unsafe/extern/asm/std/alloc/pointer
     or volatile access and no function that reads or writes a register, and no kernel, build or ISO input (Makefile,
     *.mk, linker scripts, the kernel Rust root and its #[path] modules, user/) mentions it;
  2. compile-fail controls: machine state cannot be forged from outside the crate, an abstract Event cannot be performed,
     and unsafe code is rejected;
  3. the Rust test suite (Dell fixtures, qualification versus readiness, the full abstract sequence, firmware and SMI
     refusal, timeouts, malformed/missing/contradictory facts, ordering, seeded safety property);
  4. the independent Python reference replays the seeded traces and must agree on every step, and it is shown to detect a
     corrupted trace;
  5. a bounded mutation check: each mutant changes one safety decision and must be killed by the Rust tests or by the
     reference.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import concurrent.futures as cf
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / 'kernel/rust/usb/ehci_init'
SUITE = ROOT / 'tests/rust/ehci_init_model.rs'
ORACLE = ROOT / 'scripts/ehci_init_oracle.py'
MIN_TESTS = 17

M = []


def mut(name, why, old, new, file='machine.rs'):
    M.append((name, why, old, new, file))


mut('legacy-enables-mask-wrong', 'SMI enable bit 13 (SMI on OS ownership) is not cleared', 'pub const LEGCTL_ENABLES: u32 = 0x0000_e03f;', 'pub const LEGCTL_ENABLES: u32 = 0x0000_c03f;')
mut('periodic-enable-bit-wrong', 'the periodic schedule enable is the wrong USBCMD bit', 'pub const CMD_PERIODIC_ENABLE: u32 = 1 << 4;', 'pub const CMD_PERIODIC_ENABLE: u32 = 1 << 3;')
mut('halted-bit-wrong', 'HCHalted is the wrong USBSTS bit', 'pub const STS_HALTED: u32 = 1 << 12;', 'pub const STS_HALTED: u32 = 1 << 11;')
mut('periodic-status-bit-wrong', 'the periodic schedule status is the wrong USBSTS bit', 'pub const STS_PERIODIC_ACTIVE: u32 = 1 << 14;', 'pub const STS_PERIODIC_ACTIVE: u32 = 1 << 13;')
mut('handoff-timeout-moved', 'the handoff deadline is not one second', 'pub const HANDOFF_TICKS: u64 = 1000;', 'pub const HANDOFF_TICKS: u64 = 2000;')
mut('revocation-ignored', 'firmware taking the OS semaphore back is not a failure',
    "                if !os {\n                    return self.fail(Reason::FirmwareRevoked, Rule::FaultRevoked);\n                }\n", "")
mut('handoff-stability-early', 'a release is stable without time passing', 'Some(t) if self.now > t => {', 'Some(t) if self.now >= t => {')
mut('halt-ignores-schedules', 'Run/Stop cleared while a schedule is enabled or active',
    "                if cmd & (CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE) != 0 || sts & (STS_PERIODIC_ACTIVE | STS_ASYNC_ACTIVE) != 0 {\n                    return Err(Reason::SchedulesStillActive);\n                }\n", "")
mut('reset-while-running', 'HCRESET requested without Run/Stop being clear', 'if cmd & CMD_RUN != 0 || sts & STS_HALTED == 0 {', 'if sts & STS_HALTED == 0 {')
mut('smi-write-keeps-status', 'the SMI control write carries the write-one-to-clear status bits', 'let value = ctl & 0x0000_ffff & !LEGCTL_ENABLES;', 'let value = ctl & !LEGCTL_ENABLES;')
mut('handoff-without-fresh-read', 'the OS semaphore is written from a stale observation', '                self.fresh_or(&[(F_SUP, Field::LegacyBios)])?;\n', '')
mut('reset-without-configflag', 'reset accepted without CONFIGFLAG read back clear', 'if self.reset_seen == 7 {', 'if self.reset_seen & 3 == 3 {')
mut('halt-value-keeps-run', 'the halt write does not clear Run/Stop', 'let value = cmd & !CMD_RUN;', 'let value = cmd;')
mut('host-error-ignored', 'a host system error is not a failure',
    "                if v & STS_HOST_ERROR != 0 {\n                    return self.fail(Reason::HostSystemError, Rule::FaultHostError);\n                }\n", "")
mut('inferred-accepted', 'an inferred fact satisfies a safety requirement', 'Fact::Inferred(_) => Err(Reason::InferredOnly(field)),', 'Fact::Inferred(v) => Ok(v),')
mut('companion-bound-off-by-one', 'N_PORTS equal to N_CC x N_PCC is called contradictory', 'if cc > 0 && u16::from(ports) > u16::from(cc) * u16::from(pcc) {', 'if cc > 0 && u16::from(ports) >= u16::from(cc) * u16::from(pcc) {')
mut('observed-disagreement-accepted', 'two differing observations are not contradictory', "                if a == b {\n                    Fact::Observed(a)\n                } else {\n                    Fact::Contradictory\n                }\n            }\n            (Fact::Observed(a), Fact::Inferred(_))",
    "                let _ = b;\n                Fact::Observed(a)\n            }\n            (Fact::Observed(a), Fact::Inferred(_))", file='facts.rs')

CF = {
    'state-cannot-be-forged': ("use vos_ehci_init::*;\nfn main() { let mut m = Machine::new(); m.phase = Phase::Reset; }\n", 'E0616'),
    'machine-cannot-be-built-by-hand': ("use vos_ehci_init::*;\nfn main() { let _m = Machine { writes: 9 }; }\n", 'with struct literal syntax due to private fields'),
    'event-cannot-be-performed': ("use vos_ehci_init::*;\nfn main() { Event::UsbcmdWrite { value: 0 }.perform(); }\n", 'E0599'),
}


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def strip_comments(text):
    out = []
    for line in text.splitlines():
        out.append(re.sub(r'//.*$', '', line))
    return '\n'.join(out)


BANNED = [r'\bunsafe\b', r'\bextern\b', r'\basm!', r'\bstd::', r'\balloc\b', r'core::ptr', r'volatile', r'core::arch', r'no_mangle',
          r'#\[link', r'\bfn\s+(read|write|inb|outb|outl|inl|mmio|pci)(_\w*|8|16|32)?\s*[(<]', r'\bstatic\s+mut\b', r'\bimpl\s+(Read|Write)\b']


def static_gate():
    lib = (MODEL / 'lib.rs').read_text()
    if '#![no_std]' not in lib or '#![forbid(unsafe_code)]' not in lib:
        raise SystemExit('lib.rs must be #![no_std] and #![forbid(unsafe_code)]')
    for p in sorted(MODEL.glob('*.rs')):
        code = strip_comments(p.read_text()).replace('#![forbid(unsafe_code)]', '')
        for pat in BANNED:
            if re.search(pat, code):
                raise SystemExit(f'{p.name}: forbidden construct {pat}')
    if len(list(MODEL.iterdir())) != len(list(MODEL.glob('*.rs'))):
        raise SystemExit('the model directory may only contain Rust sources')
    # nothing that builds the kernel, an ISO or a user program may mention the crate
    inputs = [ROOT / 'Makefile', ROOT / 'user', ROOT / 'arch', ROOT / 'kernel', ROOT / 'tools']
    hits = []
    for base in inputs:
        files = [base] if base.is_file() else [f for f in base.rglob('*') if f.is_file() and f.suffix in ('.rs', '.mk', '.ld', '.c', '.h', '.S', '.cfg', '.toml', '') or f.name == 'Makefile']
        for f in files:
            if MODEL in f.parents or 'build' in f.parts:
                continue
            try:
                if re.search(r'ehci_init|vos_ehci_init', f.read_text(errors='ignore')):
                    hits.append(str(f.relative_to(ROOT)))
            except OSError:
                pass
    if hits:
        raise SystemExit('kernel/build inputs mention the model: ' + ', '.join(hits))
    # the production Rust root pulls modules in only through #[path] / mod lines; none may reach the model
    for root in (ROOT / 'arch/x86_64/rust').glob('*.rs'):
        for m in re.finditer(r'#\[path\s*=\s*"([^"]+)"\]', root.read_text()):
            if 'ehci_init' in m.group(1):
                raise SystemExit(f'{root.name} includes {m.group(1)}')
    return {'files_scanned_for_mentions': [str(b.relative_to(ROOT)) for b in inputs], 'banned_patterns': len(BANNED)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--only', help='comma-separated mutant names (debugging)')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    names = [m[0] for m in M]
    if len(set(names)) != len(names):
        raise SystemExit('duplicate mutant names')
    srcs = {p.name: p.read_text() for p in MODEL.glob('*.rs')}
    for name, _why, old, new, file in M:
        if srcs[file].count(old) != 1 or old == new:
            raise SystemExit(f'mutant {name}: anchor occurs {srcs[file].count(old)} times in {file} (or no change)')
    report = {'hosted_only': True, 'device_access': False, 'sources_sha256': {}, 'commands': []}
    for p in sorted(MODEL.glob('*.rs')) + [SUITE, ORACLE, Path(__file__)]:
        report['sources_sha256'][str(p.relative_to(ROOT))] = sha(p)
    rust = [args.rustc, '--edition=2024']

    def run(argv, name, expected=0, env=None, timeout=600):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(env or {})))
        if p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (out / name).read_text()

    try:
        report['static_isolation'] = static_gate()
        rlib = out / 'libvos_ehci_init.rlib'
        run(rust + ['-Dwarnings', '--crate-name', 'vos_ehci_init', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cstrip=symbols', MODEL / 'lib.rs', '-o', rlib], 'crate-build.txt')
        # compile-fail controls
        cf_res = {}
        for name, (code, want) in CF.items():
            f = out / f'cf-{name}.rs'
            f.write_text(code)
            log = run(rust + ['--extern', f'vos_ehci_init={rlib}', f, '-o', out / f'cf-{name}'], f'cf-{name}.txt', expected=1)
            if want not in log:
                raise RuntimeError(f'compile-fail control {name}: expected {want}')
            cf_res[name] = want
        # unsafe code is rejected by the crate itself
        bad = out / 'unsafe-crate'
        shutil.copytree(MODEL, bad)
        (bad / 'lib.rs').write_text((MODEL / 'lib.rs').read_text() + '\npub fn forged(p: *const u8) -> u8 { unsafe { *p } }\n')
        log = run(rust + ['--crate-name', 'unsafe_probe', '--crate-type=rlib', bad / 'lib.rs', '-o', out / 'unsafe-probe.rlib'], 'cf-unsafe-rejected.txt', expected=1)
        if 'unsafe' not in log:
            raise RuntimeError('compile-fail control unsafe-rejected: expected an unsafe error')
        cf_res['unsafe-rejected'] = 'forbid(unsafe_code)'
        report['compile_fail_controls'] = cf_res
        # Rust tests
        tests = out / 'model-tests'
        run(rust + ['-Dwarnings', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '-Cstrip=symbols', '--extern', f'vos_ehci_init={rlib}', SUITE, '-o', tests], 'model-tests-build.txt')
        trace = out / 'trace.txt'
        text = run([tests, '--test-threads=1'], 'model-tests.txt', env={'EHCI_TRACE_OUT': str(trace)})
        m = re.search(r'test result: ok\. (\d+) passed; 0 failed', text)
        if not m or int(m.group(1)) < MIN_TESTS:
            raise RuntimeError('model tests did not pass')
        report['tests_passed'] = int(m.group(1))
        # independent reference
        run([sys.executable, '-I', '-B', ORACLE, trace], 'oracle.txt')
        report['oracle'] = (out / 'oracle.txt').read_text().strip()
        lines = trace.read_text().splitlines()
        k = next(i for i, l in enumerate(lines) if l.startswith('ok ') and ' handoff.request ' in l)
        lines[k] = lines[k].replace(' handoff.request ', ' handoff.stable ')
        (out / 'trace-corrupted.txt').write_text('\n'.join(lines) + '\n')
        run([sys.executable, '-I', '-B', ORACLE, out / 'trace-corrupted.txt'], 'oracle-corrupted.txt', expected=1)
        report['oracle_detects_corrupted_trace'] = True
        # bounded mutation check
        todo = [m for m in M if not args.only or m[0] in args.only.split(',')]
        workers = out / 'mutant-workers'
        workers.mkdir()
        free = [f's{k}' for k in range(args.jobs)]
        import threading
        lock = threading.Lock()

        def one(m):
            name, why, old, new, file = m
            with lock:
                w = free.pop()
            try:
                d = workers / w
                d.mkdir(exist_ok=True)
                md = d / 'ehci_init'
                if md.exists():
                    shutil.rmtree(md)
                shutil.copytree(MODEL, md)
                (md / file).write_text(srcs[file].replace(old, new))
                res = {'name': name, 'breaks': why, 'killed_by': []}

                def sh(argv, env=None, timeout=300):
                    return subprocess.run(list(map(str, argv)), cwd=ROOT, capture_output=True, text=True, timeout=timeout, env=dict(os.environ, **(env or {})))
                b = sh(rust + ['--cap-lints=allow', '--crate-name', 'vos_ehci_init', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cstrip=symbols', md / 'lib.rs', '-o', d / 'lib.rlib'])
                if b.returncode:
                    res['killed_by'] = ['INVALID: mutant does not compile']
                    res['detail'] = b.stderr[-300:]
                    return res
                b = sh(rust + ['--cap-lints=allow', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '-Cstrip=symbols', '--extern', f'vos_ehci_init={d / "lib.rlib"}', SUITE, '-o', d / 'tests'])
                if b.returncode:
                    res['killed_by'] = ['INVALID: tests do not compile against the mutant']
                    res['detail'] = b.stderr[-300:]
                    return res
                tr = d / 'trace.txt'
                if tr.exists():
                    tr.unlink()
                t = sh([d / 'tests', '--test-threads=1'], env={'EHCI_TRACE_OUT': str(tr)})
                if t.returncode:
                    res['killed_by'].append('rust-tests')
                    res['failing_tests'] = re.findall(r'^test (\S+) \.\.\. FAILED$', t.stdout, re.M)[:6]
                if tr.exists():
                    o = sh([sys.executable, '-I', '-B', ORACLE, tr])
                    if o.returncode:
                        res['killed_by'].append('oracle')
                return res
            finally:
                with lock:
                    free.append(w)

        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(one, todo))
        (out / 'mutants.json').write_text(json.dumps(results, indent=1) + '\n')
        invalid = [r['name'] for r in results if any(k.startswith('INVALID') for k in r['killed_by'])]
        survived = [r['name'] for r in results if not r['killed_by']]
        report['mutation_summary'] = {'total': len(results), 'killed': len(results) - len(survived) - len(invalid), 'survived': survived, 'invalid': invalid,
                                      'killed_only_by_oracle': [r['name'] for r in results if r['killed_by'] == ['oracle']]}
        report['pass'] = not survived and not invalid
    except (RuntimeError, SystemExit) as e:
        report['pass'] = False
        report['failure'] = str(e)
    (out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps({k: report.get(k) for k in ('pass', 'tests_passed', 'oracle', 'compile_fail_controls', 'mutation_summary', 'failure')}, indent=1))
    sys.exit(0 if report['pass'] else 1)


if __name__ == '__main__':
    main()

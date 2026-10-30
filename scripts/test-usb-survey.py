#!/usr/bin/env python3
"""USB input bring-up, Increment 1: hosted gate for the read-only survey (kernel/rust/usb/xhci/survey.rs).

Builds the portable xHCI crate with the survey module, runs tests/rust/xhci_survey.rs, checks that the survey traits
cannot be used to write (compile-fail controls) and that the platform sources contain no write path, then runs mutation
controls: each mutant changes one decision in survey.rs and the tests must fail. Sources are never modified; the mutants
are built in worker directories below build/. Nothing here touches PCI, MMIO or physical memory.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CRATE = ROOT / 'kernel/rust/usb/xhci'
SUITE = ROOT / 'tests/rust/xhci_survey.rs'
M = []


def mut(name, why, old, new):
    M.append((name, why, old, new))


# ---- scan ----
mut('scan-multifunction-ignored', 'every device is probed for eight functions', "let multi = (word(p, bus, dev, 0, 0x0c) >> 16) as u8 & 0x80 != 0;", "let multi = true;")
mut('scan-vendor-zero-accepted', 'a function with vendor 0 is real', "id as u16 != 0xffff && id as u16 != 0\n}", "id as u16 != 0xffff\n}")
mut('scan-all-ones-accepted', 'a function that answers all ones is real', "id as u16 != 0xffff && id as u16 != 0\n}", "id as u16 != 0\n}")
mut('scan-function-limit-moved', 'the function bound is not the bound', "if s.seen >= FUNCTION_LIMIT {", "if s.seen >= FUNCTION_LIMIT + 7 {")
mut('scan-usb-class', 'the wrong class is called USB', "0x0c03 => {", "0x0c04 => {")
mut('scan-iommu-class', 'the wrong class is called an IOMMU', "0x0806 => s.iommu_functions", "0x0807 => s.iommu_functions")
mut('scan-bridge-type', 'any header type is followed as a bridge', "0x0604 if header & 0x7f == 1 => {", "0x0604 => {")
mut('scan-bridge-upward-only', 'a bridge may send the walk back to a lower bus', "if secondary > bus && subordinate >= secondary && !queued[usize::from(secondary)] && tail < BUS_LIMIT {", "if subordinate >= secondary && !queued[usize::from(secondary)] && tail < BUS_LIMIT {")
mut('scan-bridge-subordinate', 'a reversed bus range is followed', "if secondary > bus && subordinate >= secondary && !queued", "if secondary > bus && !queued")
mut('scan-bridge-queued-twice', 'a bus named by two bridges is scanned twice', "&& !queued[usize::from(secondary)] && tail < BUS_LIMIT {", "&& tail < BUS_LIMIT {")
mut('scan-bridge-unmarked', 'a followed bus is not marked as queued', "queued[usize::from(secondary)] = true;\n                            if let Some(q)", "if let Some(q)")
mut('scan-queue-bound', 'the bus queue is larger than its array', "&& tail < BUS_LIMIT {\n                            queued", "&& tail < BUS_LIMIT + 8 {\n                            queued")
mut('scan-skip-count', 'unusable bridges are not counted', "                            s.skipped_bridges = s.skipped_bridges.saturating_add(1);", "                            let _ = 0;")
mut('scan-record-bound', 'controllers beyond the recording limit overwrite the last entry', "if let Some(slot) = s.usb.get_mut(usize::from(s.recorded)) {", "if let Some(slot) = s.usb.get_mut(usize::from(s.recorded).min(MAX_USB - 1)) {")
# ---- BAR decoding ----
mut('bar-memory-decoding-ignored', 'a controller with memory decoding off is read', "if self.command & 2 == 0 {", "if false {")
mut('bar-io-accepted', 'an I/O BAR is treated as memory', "if low & 1 != 0 {\n            return Err(\"I/O BAR\");", "if false {\n            return Err(\"I/O BAR\");")
mut('bar-64bit-high-ignored', 'the high half of a 64-bit BAR is dropped', "2 => (u64::from(self.bar1) << 32) | u64::from(low & !0xf),", "2 => u64::from(low & !0xf),")
mut('bar-below-1mib', 'a BAR below 1 MiB is read', "if base < 0x10_0000 {", "if base < 0x1000 {")
mut('bar-unaligned', 'an unaligned BAR is read', "if base % 4096 != 0 {", "if false {")
mut('bar-header-type', 'a bridge header is read as a controller', "if self.header & 0x7f != 0 {\n            return Err(\"not a type-0 header\");", "if false {\n            return Err(\"not a type-0 header\");")
# ---- PCI capabilities ----
mut('pci-cap-cycle', 'a looping capability list is followed to the bound', "if seen & bit != 0 {", "if false && seen & bit != 0 {")
mut('pci-cap-devctl-offset', 'Device Control is read from the wrong offset', "i.devctl = word(p, b, d, n, ptr + 8) as u16;", "i.devctl = word(p, b, d, n, ptr + 4) as u16;")
mut('pci-cap-msix-bit', 'the MSI-X enable bit is wrong', "i.msix_on = (hdr >> 16) & 0x8000 != 0;", "i.msix_on = (hdr >> 16) & 0x4000 != 0;")
mut('pci-cap-msi-bit', 'the MSI enable bit is wrong', "i.msi_on = (hdr >> 16) & 1 != 0;", "i.msi_on = (hdr >> 16) & 2 != 0;")
mut('pci-cap-pm-end', 'a PM capability at the end of the space is read past it', "0x01 if i.pm == 0 && ptr <= 0xf8 => {", "0x01 if i.pm == 0 => {")
mut('pci-cap-below-header', 'a capability pointer inside the header is followed', "if ptr < 0x40 {\n            return i;", "if ptr < 0x10 {\n            return i;")
mut('pci-cap-status-bit', 'the capability-list status bit is ignored', "if f.status & 0x10 == 0 {\n        return i;", "if false {\n        return i;")
# ---- xHCI registers ----
mut('xhci-caplength-low', 'a CAPLENGTH below the register block is accepted', "if op < 0x20 || op % 4 != 0 {", "if op < 0x04 || op % 4 != 0 {")
mut('xhci-caplength-alignment', 'an unaligned CAPLENGTH is accepted', "if op < 0x20 || op % 4 != 0 {", "if op < 0x20 {")
mut('xhci-all-ones', 'a controller that reads all ones is decoded', "if w0 == 0xffff_ffff {\n        return Err(\"reads as all ones\");", "if false {\n        return Err(\"reads as all ones\");")
mut('xhci-scratchpad-hi', 'the scratchpad high field is read from the wrong bits', "(((self.hcs2 >> 21) & 0x1f) << 5)", "(((self.hcs2 >> 22) & 0x1f) << 5)")
mut('xhci-scratchpad-lo', 'the scratchpad low field is read from the wrong bits', "| ((self.hcs2 >> 27) & 0x1f)", "| ((self.hcs2 >> 26) & 0x1f)")
mut('xhci-port-limit', 'the port count is not bounded', "for p in 0..x.ports.min(PORT_LIMIT) {", "for p in 0..x.ports {")
mut('xhci-port-connect-bit', 'the connection bit is wrong', "if sc & 1 != 0 {\n            x.connected", "if sc & 2 != 0 {\n            x.connected")
mut('xhci-port-ordinal', 'ports are numbered from the wrong base', "*slot = Port { index: p, portsc: sc };", "*slot = Port { index: p + 1, portsc: sc };")
mut('xhci-xecp-shift', 'the extended capability pointer is in bytes', "let mut off = (x.hcc1 >> 16) << 2;", "let mut off = (x.hcc1 >> 16) << 1;")
mut('xhci-next-units', 'the next-capability pointer is in bytes', "let next = ((h >> 8) & 0xff) << 2;", "let next = ((h >> 8) & 0xff) << 1;")
mut('xhci-cap-limit', 'the capability chain is one longer than its bound', "if usize::from(x.caps) >= CAP_LIMIT {\n            return Err(\"capability list too long\");", "if usize::from(x.caps) > CAP_LIMIT {\n            return Err(\"capability list too long\");")
mut('xhci-legacy-first-only', 'a second legacy capability replaces the first', "1 if x.legacy.is_none() =>", "1 =>")
mut('xhci-debug-first-only', 'a second debug capability replaces the first', "10 if x.dbc.is_none() =>", "10 =>")
mut('xhci-debug-offset', 'the debug control register is read from the wrong offset', "x.dbc = Some((off as u16, r.word(off + 0x20)?))", "x.dbc = Some((off as u16, r.word(off + 0x1c)?))")
mut('xhci-protocol-name', 'a protocol capability with another name is listed', "if name == u32::from_le_bytes(*b\"USB \")\n                    && let Some(s)", "if let Some(s)")
mut('xhci-protocol-major', 'the protocol major revision is wrong', "Proto { major: (h >> 24) as u8, minor: (h >> 16) as u8,", "Proto { major: (h >> 23) as u8, minor: (h >> 16) as u8,")
mut('xhci-protocol-count', 'the protocol port count is wrong', "count: (ports >> 8) as u8 }", "count: (ports >> 9) as u8 }")
mut('xhci-window-bound', 'a register past the window end is requested', "if off > WINDOW - 4 {", "if off > WINDOW {")
mut('xhci-second-read-skipped', 'the second reading of USBCMD is not a reading', "x.usbcmd2 = r.word(op)?;", "x.usbcmd2 = x.usbcmd;")
mut('xhci-second-status-skipped', 'the second reading of USBSTS is not a reading', "x.usbsts2 = r.word(op + 4)?;", "x.usbsts2 = x.usbsts;")
mut('xhci-refusal-continues', 'a refused port read does not end the walk', "let sc = r.word(op + 0x400 + 0x10 * u32::from(p))?;", "let sc = r.word(op + 0x400 + 0x10 * u32::from(p)).unwrap_or(0);")
# ---- rendering ----
mut('line-capacity', 'a line can outgrow the room kept for its terminator', "if self.n < self.b.len() - 2\n            && let Some(slot)", "if self.n < self.b.len()\n            && let Some(slot)")
mut('line-printable', 'unprintable bytes reach the console', "*slot = if c.is_ascii_graphic() || c == b' ' { c } else { b'?' };", "*slot = c;")
mut('line-terminator', 'a line is not terminated', "for crlf in [b'\\r', b'\\n'] {", "for crlf in [b'\\r'] {")
mut('hex-width', 'hexadecimal values lose their leading zeros', "if nib != 0 || started || i < digits || i == 0 {", "if nib != 0 || started || i == 0 {")
mut('render-changed-warning', 'a controller that changes by itself is not flagged', "if x.usbcmd != x.usbcmd2 || x.usbsts != x.usbsts2 {", "if false {")
mut('render-incomplete-note', 'an incomplete scan is not flagged', "if s.limit_hit || s.skipped_bridges != 0 || s.usb_total > u16::from(s.recorded) {", "if false {")
mut('summary-not-read', 'a controller that was not read is summarised as if it were', "if x.reads == 0 {\n        l.s(\" NOT READ\");", "if false {\n        l.s(\" NOT READ\");")

CF = {
    'pci-trait-has-no-write': ('use vos_xhci::xhci::survey::*;\nfn f(p: &mut impl Pci) { p.write32(0, 0, 0, 0, 1); }\nfn main() {}\n', 'E0599'),
    'mmio-trait-has-no-write': ('use vos_xhci::xhci::survey::*;\nfn f(m: &mut impl Mmio) { m.write32(0, 1); }\nfn main() {}\n', 'E0599'),
    'scan-needs-a-pci-reader': ('use vos_xhci::xhci::survey::*;\nfn main() { let _ = scan(&mut 5u32); }\n', 'E0277'),
}
BANNED = re.compile(r'\bunsafe\b|\bextern\b|\basm!|\bglobal_asm!|\balloc::|\bstd::|\bstatic\s+mut\b')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--only', help='comma-separated mutant names (debugging)')
    ap.add_argument('--workers-dir', type=Path, help='reuse these mutant build directories in place (build caches, not evidence)')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    names = [m[0] for m in M]
    if len(set(names)) != len(names):
        raise SystemExit('duplicate mutant names')
    src = (CRATE / 'survey.rs').read_text()
    for name, _why, old, new in M:
        if src.count(old) != 1 or old == new:
            raise SystemExit(f'mutant {name}: anchor occurs {src.count(old)} times (or no change)')
    report = {'hosted_only': True, 'device_access': False, 'source_sha256': {}, 'commands': []}
    for p in sorted(CRATE.glob('*.rs')) + [SUITE, Path(__file__), ROOT / 'arch/x86_64/xhci_survey.c', ROOT / 'arch/x86_64/rust/xhci_survey.rs']:
        report['source_sha256'][str(p.relative_to(ROOT))] = sha(p)
    for p in sorted(CRATE.glob('*.rs')):
        if BANNED.search(p.read_text()):
            raise SystemExit('nonportable/unsafe source: ' + str(p))
    rust = [args.rustc, '--edition=2024']

    def run(argv, name, expected=0, env=None, timeout=900):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout,
                               env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(env or {})))
        if p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (out / name).read_text()

    try:
        rlib = out / 'libvos_xhci.rlib'
        run(rust + ['-Dwarnings', '--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cstrip=symbols', CRATE / 'lib.rs', '-o', rlib], 'crate-build.txt')
        run(rust + ['-Dwarnings', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '-Cstrip=symbols', '--extern', f'vos_xhci={rlib}', SUITE, '-o', out / 'survey-tests'], 'tests-build.txt')
        text = run([out / 'survey-tests', '--test-threads=1'], 'tests.txt')
        m = re.search(r'test result: ok\. (\d+) passed; 0 failed', text)
        if not m:
            raise RuntimeError('survey tests did not pass')
        report['tests_passed'] = int(m.group(1))
        # compile-fail controls
        cf = {}
        for name, (code, want) in CF.items():
            f = out / f'cf-{name}.rs'
            f.write_text(code)
            log = run(rust + ['--extern', f'vos_xhci={rlib}', f, '-o', out / f'cf-{name}'], f'cf-{name}.txt', expected=1)
            if want not in log:
                raise RuntimeError(f'compile-fail control {name}: expected {want}')
            cf[name] = want
        report['compile_fail_controls'] = cf
        # mutation controls
        todo = [m for m in M if not args.only or m[0] in args.only.split(',')]
        workers = args.workers_dir.resolve() if args.workers_dir else out / 'mutant-workers'
        if args.workers_dir:
            if not workers.is_relative_to(ROOT / 'build'):
                raise RuntimeError('--workers-dir must be below build/')
            report['workers_dir_reused'] = str(workers.relative_to(ROOT))
        workers.mkdir(exist_ok=True)
        free = [f's{k}' for k in range(args.jobs)]
        lock = threading.Lock()

        def one(m):
            name, why, old, new = m
            with lock:
                w = free.pop()
            try:
                d = workers / w
                d.mkdir(exist_ok=True)
                md = d / 'xhci'
                if md.exists():
                    for f in CRATE.glob('*.rs'):
                        (md / f.name).write_text(f.read_text())
                else:
                    shutil.copytree(CRATE, md, ignore=shutil.ignore_patterns('target'))
                (md / 'survey.rs').write_text(src.replace(old, new))
                res = {'name': name, 'breaks': why, 'killed_by': [], 'failing_tests': []}

                def sh(argv, timeout=300):
                    return subprocess.run(list(map(str, argv)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
                b = sh(rust + ['--cap-lints=allow', '--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cstrip=symbols', md / 'lib.rs', '-o', d / 'lib.rlib'])
                if b.returncode:
                    res.update(killed_by=['INVALID: mutant does not compile'], detail=b.stderr[-300:])
                    return res
                b = sh(rust + ['--cap-lints=allow', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '-Cstrip=symbols', '--extern', f'vos_xhci={d / "lib.rlib"}', SUITE, '-o', d / 'tests'])
                if b.returncode:
                    res.update(killed_by=['INVALID: tests do not compile against the mutant'], detail=b.stderr[-300:])
                    return res
                try:
                    t = sh([d / 'tests', '--test-threads=1'], timeout=300)
                    if t.returncode:
                        res['killed_by'].append('rust-tests')
                        res['failing_tests'] = re.findall(r'^test (\S+) \.\.\. FAILED$', t.stdout, re.M)[:6]
                except subprocess.TimeoutExpired:
                    res['killed_by'].append('timeout')
                return res
            finally:
                with lock:
                    free.append(w)

        with ThreadPoolExecutor(args.jobs) as pool:
            results = list(pool.map(one, todo))
        survived = [r['name'] for r in results if not r['killed_by']]
        invalid = [r['name'] for r in results if any(k.startswith('INVALID') for k in r['killed_by'])]
        report['mutation_controls'] = results
        report['mutation_summary'] = {'total': len(results), 'killed': len(results) - len(survived), 'survived': survived, 'invalid': invalid}
        if survived or invalid:
            raise RuntimeError(f'mutation controls: survived={survived} invalid={invalid}')
        report['pass'] = True
    finally:
        report.setdefault('pass', False)
        (out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps({k: report[k] for k in ('pass', 'tests_passed', 'compile_fail_controls', 'mutation_summary') if k in report}, indent=1))


if __name__ == '__main__':
    main()

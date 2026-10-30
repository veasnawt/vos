#!/usr/bin/env python3
"""USB input bring-up, Increment 2: hosted gate for the read-only firmware-table lease (tests/host/usb-firmware-lease.c).

Builds the test against the real arch/x86_64/devmap.c, pmm.c and xhci_survey.c with AddressSanitizer and UndefinedBehaviorSanitizer
(no recovery: any report aborts), runs it, then runs mutation controls: each mutant changes one decision in devmap.c (a private
copy under a temporary directory; the source tree is never modified) and the test must fail. A mutant that still passes, or does not
compile, fails the gate. Nothing here touches PCI, MMIO or physical memory; the lease is modelled and never dereferenced."""
import argparse, hashlib, json, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEVMAP = ROOT / 'arch/x86_64/devmap.c'
TEST = 'tests/host/usb-firmware-lease.c'
M = []


def mut(name, why, old, new):
    M.append((name, why, old, new))


# The eight controls of the approved plan
mut('leaf-writable', 'the lease leaf is writable', "(pa + off) | PTE_P | (ro ? 0 : PTE_W) | PTE_NX | uc_bits;", "(pa + off) | PTE_P | PTE_W | PTE_NX | uc_bits;")
mut('drop-1gib-rule', 'a lease below the direct map is accepted', "r->pa < (1ULL << 30) || !devmap_firmware_range_ok(r->pa, r->length))", "!devmap_firmware_range_ok(r->pa, r->length))")
mut('drop-memory-map-rule', 'a lease outside firmware memory is accepted', "r->pa < (1ULL << 30) || !devmap_firmware_range_ok(r->pa, r->length))", "r->pa < (1ULL << 30))")
mut('drop-ram-overlap', 'a page that RAM also claims counts as firmware memory', "(e->type == 1 || e->type == 5) && page <", "(e->type == 5) && page <")
mut('bad-memory-is-firmware', 'bad memory counts as firmware memory', "(e->type == 1 || e->type == 5) && page <", "(e->type == 1) && page <")
mut('drop-devmap-overlap', 'a lease may overlap an existing mapping', "            if (r->pa < map[i].end && map[i].pa < r->pa + r->length) return DEVMAP_EOVERLAP;\n        uint64_t fstart", "            (void)i;\n        uint64_t fstart")
mut('drop-maxphys', 'a lease beyond MAXPHYADDR is accepted', "        if (r->pa + r->length > (1ULL << maxphys)) return DEVMAP_ERANGE;\n        for (unsigned i = 0; i < count; i++)\n            if (r->pa < map[i].end", "        for (unsigned i = 0; i < count; i++)\n            if (r->pa < map[i].end")
mut('drop-size-bound', 'a lease larger than 16 KiB is accepted', "r->length > DEVMAP_FIRMWARE_MAX_BYTES || ", "")
# Extra controls on the consistency between the leaf and the registry
mut('registry-ignores-ro', 'the registry expects a writable leaf', "(map[i].ro ? 0 : PTE_W)", "PTE_W")
mut('range-ok-overflow', 'a range that wraps counts as firmware memory', " || len > (1ULL << 20) || pa + len < pa) return 0;", " || len > (1ULL << 20)) return 0;")
mut('range-ok-empty', 'an empty range counts as firmware memory', "if (!ready || !len || len > (1ULL << 20)", "if (!ready || len > (1ULL << 20)")
mut('registry-not-recorded-ro', 'the registry does not record the lease as read-only', "map[count++] = (struct devmap_entry){pa, pa + length, start, uc_bits, what, ro};", "map[count++] = (struct devmap_entry){pa, pa + length, start, uc_bits, what, 0};")
REQUIRED = 8


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    src = DEVMAP.read_text()
    names = [m[0] for m in M]
    if len(set(names)) != len(names):
        raise SystemExit('duplicate mutant names')
    for name, _why, old, new in M:
        if src.count(old) != 1 or old == new:
            raise SystemExit(f'mutant {name}: anchor occurs {src.count(old)} times (or no change)')
    report = {'hosted_only': True, 'device_access': False, 'sources_sha256': {str(p.relative_to(ROOT)): sha(p) for p in (DEVMAP, ROOT / TEST, Path(__file__), ROOT / 'arch/x86_64/xhci_survey.c', ROOT / 'arch/x86_64/pmm.c')},
              'commands': [], 'required_mutants': REQUIRED}
    base = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-fno-pie', '-no-pie', '-Iarch/x86_64']
    san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    try:
        cmd = base + ['-Werror'] + san + [TEST, '-o', str(out / 'sanitizer')]
        report['commands'].append(cmd)
        with (out / 'sanitizer-build.txt').open('x') as f:
            b = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
        if b.returncode:
            raise RuntimeError('the test does not build; see sanitizer-build.txt')
        with (out / 'sanitizer.txt').open('x') as f:
            r = subprocess.run([str(out / 'sanitizer')], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=300)
        text = (out / 'sanitizer.txt').read_text()
        if r.returncode or 'PASS:' not in text or 'runtime error' in text or 'Sanitizer' in text:
            raise RuntimeError('the test did not pass cleanly; see sanitizer.txt')
        report['sanitizer'] = text.strip().splitlines()[-1]
        results = []
        with tempfile.TemporaryDirectory() as tmp:
            for name, why, old, new in M:
                path = Path(tmp) / f'{name}.c'
                path.write_text(src.replace(old, new))
                exe = Path(tmp) / name
                c = subprocess.run(base + san + [f'-DDEVMAP_C="{path}"', TEST, '-o', str(exe)], cwd=ROOT, capture_output=True, text=True)
                res = {'name': name, 'breaks': why}
                if c.returncode:
                    res.update(killed_by=['INVALID: mutant does not compile'], detail=c.stderr[-400:])
                else:
                    t = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=300)
                    res['killed_by'] = ['test'] if t.returncode else []
                    res['exit'] = t.returncode
                    res['first_failure'] = next((l for l in (t.stderr + t.stdout).splitlines() if l.startswith('FAIL ') or 'runtime error' in l or 'ERROR: ' in l or 'rejected(' in l), None)
                results.append(res)
                exe.unlink(missing_ok=True)
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
    print(json.dumps({k: report[k] for k in ('pass', 'sanitizer', 'mutation_summary') if k in report}, indent=1))


if __name__ == '__main__':
    main()

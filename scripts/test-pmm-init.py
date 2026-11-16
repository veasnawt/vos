#!/usr/bin/env python3
"""R4-M1H: pmm_init_in small-instance memory-safety hardening gate.
- Scope: removing the one bounded low-memory clear from the hardened reference must reproduce the accepted main pmm.c byte for
  byte (tests/reference/pmm-r4m1h.json); the frozen oracle's helpers and pmm_init_in (tests/reference/pmm-r4m1.c) must be
  byte-identical to the accepted main's. Since R4-M3, pmm_init_in is Rust (kernel/rust/mm/pmm_init.rs): the hardened C is
  frozen in tests/reference/pmm-r4m3.c (whose init functions must equal R4-M2's hardened reference), production pmm.c must no
  longer define them, and the Rust core must hold the bounded clear exactly once. The candidate is the production Rust.
- tests/host/pmm-init-regression.c, in a release build and an ASan/UBSan build (the production Rust PMM checks linked as an
  overflow-checked, debug-asserted host build): a negative control in which the frozen C faults on a guard page at exactly the
  first byte past an exact 64/128/192-frame bitmap, in a forked child; small instances equal to the frozen C on every in-bounds
  word and field; instances from 256 frames to PMM_MAX_FRAMES identical word for word; refused initialization identical and
  without bitmap writes. Same digest in both builds, 0 mismatches, 3/3 negative-control faults.
- Named mutation controls on the production Rust core (the R4-M1H set, statement for statement), each of which must mismatch or fault.
Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
PMM = 'arch/x86_64/pmm.c'; FROZEN = 'tests/reference/pmm-r4m1.c'; MANIFEST = 'tests/reference/pmm-r4m1h.json'; HARNESS = 'tests/host/pmm-init-regression.c'
GLUE = 'arch/x86_64/rust/pmm.rs'; TGLUE = 'arch/x86_64/rust/pmm_transition.rs'; HARDENED = 'tests/reference/pmm-r4m2.c'
IGLUE = 'arch/x86_64/rust/pmm_init.rs'; ICORE = 'kernel/rust/mm/pmm_init.rs'; R4M3 = 'tests/reference/pmm-r4m3.c'
RUST_LINE = 'let low = if limit < LOW_LIMIT { limit } else { LOW_LIMIT };'
INIT_FUNCTIONS = ('count_bits', 'set_frames', 'clear_frames', 'protect', 'pmm_init_in')
def function(text, name):
    """The definition of `name` in C source: from its signature line to its closing brace at column 0."""
    i = text.index(name + '('); i = text.rindex('\n', 0, i) + 1
    return text[i:text.index('\n}\n', i) + 3]
PUBLIC = ('pmm_init_in pmm_alloc_in pmm_free_check_in pmm_free_in pmm_reserve_in pmm_frame_state_in pmm_check_in pmm_init pmm_alloc_frame '
          'pmm_free_frame pmm_reserve_frame pmm_free_count pmm_kernel pmm_strerror boot_mmap_entry').split()
OLD = '    /* 5. Defensive: nothing below 1 MiB, whatever the inputs said. */\n    clear_frames(avail, 0, PMM_LOW_LIMIT / PAGE_SIZE);\n'
NEW = ('    /* 5. Defensive: nothing below 1 MiB, whatever the inputs said. Only the tracked frames: an instance smaller than\n'
       '     *    1 MiB has PMM_WORDS(frames) words. min(limit, 1 MiB) / PAGE_SIZE = min(frames, PMM_LOW_LIMIT / PAGE_SIZE). (R4-M1H) */\n'
       '    clear_frames(avail, 0, (limit < PMM_LOW_LIMIT ? limit : PMM_LOW_LIMIT) / PAGE_SIZE);\n')
LINE = 'clear_frames(avail, 0, (limit < PMM_LOW_LIMIT ? limit : PMM_LOW_LIMIT) / PAGE_SIZE);'
SEEDED, MUTANT_SEEDED = 20000, 4000
# Equivalent and excluded, because unobservable at pmm_init_in's boundary (the same analysis applies to the Rust statements since R4-M3):
# - the low-memory clear alone dropped, or the free pass's 1 MiB clamp alone dropped: each of the two redundant protections
#   covers the other (only both together leave frames below 1 MiB allocatable; that mutant is detected);
# - clearing one frame fewer (limit - PAGE_SIZE, or 255 frames for large instances), and `<=` at the 1 MiB threshold: those frames are
#   never allocatable before the clear, so clearing them or not gives the same bitmap;
# - counting usable frames from word 1: words 0-3 (below 1 MiB) are always zero.
ZA = '    while w < words {\n        if m.set_avail(w, 0).is_none() {'; ZL = '    while w < words {\n        if m.set_alloc(w, 0).is_none() {'
LOWCLEAR = '    if change_frames(m, 0, low / PAGE_SIZE, false).is_none() {\n        return ECORRUPT;\n    }\n'
MUTATIONS = [  # (name, [(exact text, replacement), ...]) on the Rust core: each must make the regression mismatch or fault
 ('restore-unbounded-clear', [(RUST_LINE, 'let low = LOW_LIMIT;')]),
 ('off-by-one-small-instance', [(RUST_LINE, 'let low = if limit < LOW_LIMIT { limit.wrapping_add(PAGE_SIZE) } else { LOW_LIMIT };')]),
 ('off-by-one-large-instance', [(RUST_LINE, 'let low = if limit < LOW_LIMIT { limit } else { LOW_LIMIT + PAGE_SIZE };')]),
 ('boundary-threshold-128', [(RUST_LINE, 'let low = if limit < LOW_LIMIT / 2 { limit } else { LOW_LIMIT };')]),
 ('boundary-max-not-min', [(RUST_LINE, 'let low = if limit > LOW_LIMIT { limit } else { LOW_LIMIT };')]),
 ('boundary-512', [(RUST_LINE, 'let low = if limit < LOW_LIMIT { limit } else { 2 * LOW_LIMIT };')]),
 ('boundary-frames-as-bytes', [('change_frames(m, 0, low / PAGE_SIZE, false)', 'change_frames(m, 0, low / 64, false)')]),
 ('zeroing-avail-short', [(ZA, ZA.replace('while w < words', 'while w.wrapping_add(1) < words'))]),
 ('zeroing-alloc-short', [(ZL, ZL.replace('while w < words', 'while w.wrapping_add(1) < words'))]),
 ('zeroing-avail-dropped', [(ZA, ZA.replace('while w < words', 'while w < 0'))]),
 ('low-memory-unprotected', [('let mut s = if base < LOW_LIMIT { LOW_LIMIT } else { base };', 'let mut s = base;'), (LOWCLEAR, '    let _ = low;\n')]),
 ('large-protect-rounding', [('let e = if end >= limit { limit } else { end.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1) };', 'let e = if end >= limit { limit } else { end & !(PAGE_SIZE - 1) };')]),
 ('large-free-inward-rounding', [('s = s.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1); // inward', 's &= !(PAGE_SIZE - 1); // inward')]),
 ('large-overflowing-entry', [('        if end < base {\n            end = u64::MAX; // overflowing: extends to 2^64\n        }\n', '        if end < base {\n            continue;\n        }\n')]),
 ('large-ignored-bytes', [('let from = if base > limit { base } else { limit };', 'let from = limit;')]),
 ('large-reservation-pass', [('        if r >= count {\n            break;', '        if r.wrapping_add(1) >= count {\n            break;')]),
 ('large-usable-count', [('    while w < words {\n        let Some(word) = m.avail(w)', '    while w.wrapping_add(1) < words {\n        let Some(word) = m.avail(w)')]),
 ('large-limit-clip', [('        if end > limit {\n            end = limit;\n        }\n', '        if end > limit.wrapping_add(PAGE_SIZE) {\n            end = limit;\n        }\n')]),
 ('error-max-frames', [('frames > MAX_FRAMES {', 'frames > MAX_FRAMES + 64 {')]),
 ('error-entry-size', [('Some(size) if size >= ENTRY_MIN && size % 8 == 0', 'Some(size) if size >= ENTRY_MIN && size % 4 == 0')]),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rustc', required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build'), 'fresh confined output')
    out.mkdir(parents=True)
    report = {'status': 'RUNNING', 'commands': []}
    def run(cmd, name, timeout=1800, check=True):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = p.returncode
        if check: need(p.returncode == 0, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        # scope: the R4-M1H change is exactly one bounded clear. The frozen hardened main (tests/reference/pmm-r4m2.c, R4-M2's
        # reference) is the accepted R4-M1 main pmm.c plus exactly that statement; production pmm_init_in and its helpers are
        # still byte-identical to it (R4-M2 moved other functions out of pmm.c, not these); the vulnerable oracle's are the
        # accepted ones.
        man = json.loads((ROOT / MANIFEST).read_text()); prod = (ROOT / PMM).read_text(); hard = (ROOT / HARDENED).read_text()
        hman = json.loads((ROOT / 'tests/reference/pmm-r4m2.json').read_text())
        need(sha(ROOT / HARDENED) == hman['sources_sha256']['pmm-r4m2.c'], 'frozen hardened source')
        need(hard.count(NEW) == 1, 'the bounded clear is present once in the hardened reference')
        need(hashlib.sha256(hard.replace(NEW, OLD).encode()).hexdigest() == man['accepted_pmm_c']['sha256'], 'hardened pmm.c = accepted R4-M1 main pmm.c + exactly the bounded clear')
        r4m3 = (ROOT / R4M3).read_text(); r4m3man = json.loads((ROOT / 'tests/reference/pmm-r4m3.json').read_text())
        need(sha(ROOT / R4M3) == r4m3man['sources_sha256']['pmm-r4m3.c'] and all(function(r4m3, f) == function(hard, f) for f in INIT_FUNCTIONS), 'the R4-M3 frozen init functions equal the hardened reference')
        need(not any(re.search(r'^[a-z].*\b%s\(' % f, prod, re.M) for f in INIT_FUNCTIONS), 'production pmm.c no longer defines pmm_init_in or its helpers (R4-M3)')
        core = (ROOT / ICORE).read_text(); need(core.count(RUST_LINE) == 1 and core.count('change_frames(m, 0, low / PAGE_SIZE, false)') == 1, 'the Rust core holds the bounded clear exactly once')
        frozen = (ROOT / FROZEN).read_text(); ref = json.loads((ROOT / 'tests/reference/pmm-r4m1.json').read_text())
        need(sha(ROOT / FROZEN) == ref['sources_sha256']['pmm-r4m1.c'], 'frozen accepted source')
        acc = hard.replace(NEW, OLD)
        need(all(function(frozen, f) == function(acc, f) for f in INIT_FUNCTIONS), 'frozen helpers and pmm_init_in equal the accepted main')
        report['scope'] = {'accepted_pmm_c_sha256': man['accepted_pmm_c']['sha256'], 'hardened_pmm_c_sha256': sha(ROOT / HARDENED), 'r4m3_frozen_pmm_c_sha256': sha(ROOT / R4M3),
                           'production_pmm_c_sha256': sha(ROOT / PMM), 'production_pmm_init_in': 'Rust (' + ICORE + ')', 'changed_statements': 1, 'init_functions_identical': list(INIT_FUNCTIONS)}
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        def host_rust(tag, d, lints='-Dwarnings'):
            # the production Rust PMM code: R4-M1 checks, R4-M2 transitions and (from `d`) the R4-M3 initialization under test
            root = out / ('root-%s.rs' % tag); lib = out / ('libpmm-host-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / GLUE)) + '] mod pmm;\n#[path=' + json.dumps(str(ROOT / TGLUE)) + '] mod pmm_transition;\n'
                            '#[path=' + json.dumps(str(d / IGLUE)) + '] mod pmm_init;\n'
                            'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n'
                            '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', lints, '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
                 '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib, root
        rust, _ = host_rust('production', ROOT)
        shim = out / 'entry-shim.c'     # the Rust pmm_init_in calls boot_mmap_entry: the candidate side's entry lookup
        shim.write_text('#include "pmm.h"\n#include "multiboot2.h"\nconst struct mb2_mmap_entry *cand_boot_mmap_entry(const struct boot_info *, uint32_t);\n'
                        'const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi, uint32_t i) { return cand_boot_mmap_entry(bi, i); }\n')
        def regression(tag, flags, rust, seeds, timeout=1800):
            r, c, b = out / ('ref-%s.o' % tag), out / ('cand-%s.o' % tag), out / ('reg-%s' % tag)
            if not r.exists(): run(['gcc', *flags, *warn, *renames, '-c', FROZEN, '-o', r], 'ref-%s-build.txt' % tag)
            run(['gcc', *flags, *warn, '-Dboot_mmap_entry=cand_boot_mmap_entry', '-c', PMM, '-o', c], 'cand-%s-build.txt' % tag)
            run(['gcc', *flags, *warn, HARNESS, r, c, shim, rust, *link, '-o', b], 'reg-%s-build.txt' % tag)
            try: text = run([b, seeds], 'reg-%s.txt' % tag, timeout=timeout, check=False)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b, c
            m = re.search(r'PMM INIT REGRESSION cases=(\d+) checks=(\d+) mismatches=(\d+) negative_faults=(\d)/3 digest=([0-9a-f]{16})', text)
            return text, (dict(cases=int(m[1]), checks=int(m[2]), mismatches=int(m[3]), negative_faults=int(m[4]), digest=m[5]) if m else None), b, c
        report['regression'] = {}; cov = {}
        for tag, flags in (('release', ['-O2']), ('sanitizer', san)):
            text, r, _, _ = regression(tag, flags, rust, SEEDED)
            need(r and r['mismatches'] == 0 and r['negative_faults'] == 3 and 'MISMATCH' not in text and 'runtime error' not in text,
                 'fixed init equals the frozen C where defined and the frozen C faults (%s): %s' % (tag, text[-600:]))
            need(text.count('frozen-C=fault fault_offset=bitmap_end') == 3, 'negative control at the exact bitmap end (%s)' % tag)
            cov[tag] = re.search(r'^PMM INIT COVERAGE .*$', text, re.M)[0]; report['regression'][tag] = r
            report['negative_control_' + tag] = re.findall(r'^PMM INIT NEGATIVE CONTROL .*$', text, re.M)
        need(report['regression']['release']['digest'] == report['regression']['sanitizer']['digest'] and cov['release'] == cov['sanitizer'], 'transcript independent of build mode')
        counts = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', cov['release']))
        need(all(counts[k] > 0 for k in ('small', 'large', 'max', 'refused', 'seeded', 'frame256', 'ignored', 'reserved', 'overflow')), 'every class reached')
        report['coverage'] = cov['release']
        # named mutation controls (release; the frozen object is shared)
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 18, 'at least 18 named mutation controls')
        def mutant(m):
            name, edits = m
            d = out / 'mutants' / name
            for f in (ICORE, IGLUE): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            src = (d / ICORE).read_text()
            for old, new in edits:
                need(src.count(old) == 1, 'unique mutation ' + name); src = src.replace(old, new)
            (d / ICORE).write_text(src)
            lib, root = host_rust('mutant-' + name, d, '--cap-lints=allow')
            text, r, b, c = regression('mutant-' + name, ['-O1', '-Iarch/x86_64'], lib, MUTANT_SEEDED, timeout=600)
            ex = next((x['exit'] for x in report['commands'] if x['log'] == 'reg-mutant-%s.txt' % name), None)
            shutil.rmtree(d)
            for f in (b, c, lib, root): f.unlink(missing_ok=True)
            return name, r, text, ex
        (out / 'ref-mutant.o').unlink(missing_ok=True)
        run(['gcc', '-O1', '-Iarch/x86_64', *warn, *renames, '-c', FROZEN, '-o', out / 'ref-mutant-shared.o'], 'ref-mutant-build.txt')
        for m in MUTATIONS: shutil.copyfile(out / 'ref-mutant-shared.o', out / ('ref-mutant-%s.o' % m[0]))
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, len(os.sched_getaffinity(0))))) as pool:
            results = list(pool.map(mutant, MUTATIONS))
        missed = [n for n, r, _, _ in results if r is not None and r['mismatches'] == 0 and r['negative_faults'] == 3]
        need(not missed, 'mutations not detected: %s' % missed)
        for name, r, text, ex in results:
            report['mutations'][name] = {'detected': True, 'mismatches': r['mismatches'] if r else ('hang' if text == 'TIMEOUT' else 'abnormal exit (fault)'), 'exit': ex}
            (out / ('ref-mutant-%s.o' % name)).unlink(missing_ok=True)
        report['source_sha256'] = {n: sha(ROOT / n) for n in [PMM, FROZEN, HARDENED, R4M3, MANIFEST, HARNESS, GLUE, TGLUE, IGLUE, ICORE, 'scripts/test-pmm-init.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:4000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

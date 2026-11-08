#!/usr/bin/env python3
"""R4-M1H: pmm_init_in small-instance memory-safety hardening gate.
- Scope: removing the one bounded low-memory clear from the production arch/x86_64/pmm.c must reproduce the accepted main
  pmm.c byte for byte (tests/reference/pmm-r4m1h.json), and the frozen oracle's helpers and pmm_init_in
  (tests/reference/pmm-r4m1.c) must be byte-identical to the accepted main's.
- tests/host/pmm-init-regression.c, in a release build and an ASan/UBSan build (the production Rust PMM checks linked as an
  overflow-checked, debug-asserted host build): a negative control in which the frozen C faults on a guard page at exactly the
  first byte past an exact 64/128/192-frame bitmap, in a forked child; small instances equal to the frozen C on every in-bounds
  word and field; instances from 256 frames to PMM_MAX_FRAMES identical word for word; refused initialization identical and
  without bitmap writes. Same digest in both builds, 0 mismatches, 3/3 negative-control faults.
- Named mutation controls on the production pmm.c, each of which must mismatch or fault.
Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
PMM = 'arch/x86_64/pmm.c'; FROZEN = 'tests/reference/pmm-r4m1.c'; MANIFEST = 'tests/reference/pmm-r4m1h.json'; HARNESS = 'tests/host/pmm-init-regression.c'
GLUE = 'arch/x86_64/rust/pmm.rs'
PUBLIC = ('pmm_init_in pmm_alloc_in pmm_free_check_in pmm_free_in pmm_reserve_in pmm_frame_state_in pmm_check_in pmm_init pmm_alloc_frame '
          'pmm_free_frame pmm_reserve_frame pmm_free_count pmm_kernel pmm_strerror boot_mmap_entry').split()
OLD = '    /* 5. Defensive: nothing below 1 MiB, whatever the inputs said. */\n    clear_frames(avail, 0, PMM_LOW_LIMIT / PAGE_SIZE);\n'
NEW = ('    /* 5. Defensive: nothing below 1 MiB, whatever the inputs said. Only the tracked frames: an instance smaller than\n'
       '     *    1 MiB has PMM_WORDS(frames) words. min(limit, 1 MiB) / PAGE_SIZE = min(frames, PMM_LOW_LIMIT / PAGE_SIZE). (R4-M1H) */\n'
       '    clear_frames(avail, 0, (limit < PMM_LOW_LIMIT ? limit : PMM_LOW_LIMIT) / PAGE_SIZE);\n')
LINE = 'clear_frames(avail, 0, (limit < PMM_LOW_LIMIT ? limit : PMM_LOW_LIMIT) / PAGE_SIZE);'
SEEDED, MUTANT_SEEDED = 20000, 4000
# Equivalent and excluded, because unobservable at pmm_init_in's boundary:
# - the low-memory clear alone dropped, or the free pass's 1 MiB clamp alone dropped: each of the two redundant protections
#   covers the other (only both together leave frames below 1 MiB allocatable; that mutant is detected);
# - clearing one frame fewer (limit - PAGE_SIZE, or 255 frames for large instances), and `<=` at the 1 MiB threshold: those frames are
#   never allocatable before the clear, so clearing them or not gives the same bitmap;
# - counting usable frames from word 1: words 0-3 (below 1 MiB) are always zero.
MUTATIONS = [  # (name, [(exact text, replacement), ...]): each must make the regression mismatch or fault
 ('restore-unbounded-clear', [(LINE, 'clear_frames(avail, 0, PMM_LOW_LIMIT / PAGE_SIZE);')]),
 ('off-by-one-small-instance', [('(limit < PMM_LOW_LIMIT ? limit :', '(limit < PMM_LOW_LIMIT ? limit + PAGE_SIZE :')]),
 ('off-by-one-large-instance', [(': PMM_LOW_LIMIT) / PAGE_SIZE);', ': PMM_LOW_LIMIT + PAGE_SIZE) / PAGE_SIZE);')]),
 ('boundary-threshold-128', [('limit < PMM_LOW_LIMIT ?', 'limit < PMM_LOW_LIMIT / 2 ?')]),
 ('boundary-max-not-min', [('limit < PMM_LOW_LIMIT ?', 'limit > PMM_LOW_LIMIT ?')]),
 ('boundary-512', [(': PMM_LOW_LIMIT) / PAGE_SIZE);', ': 2 * PMM_LOW_LIMIT) / PAGE_SIZE);')]),
 ('boundary-frames-as-bytes', [(': PMM_LOW_LIMIT) / PAGE_SIZE);', ': PMM_LOW_LIMIT) / 64);')]),
 ('zeroing-avail-short', [('memset(avail, 0, PMM_WORDS(frames) * sizeof(uint64_t));', 'memset(avail, 0, (PMM_WORDS(frames) - 1) * sizeof(uint64_t));')]),
 ('zeroing-alloc-short', [('memset(alloc, 0, PMM_WORDS(frames) * sizeof(uint64_t));', 'memset(alloc, 0, (PMM_WORDS(frames) - 1) * sizeof(uint64_t));')]),
 ('zeroing-avail-dropped', [('memset(avail, 0, PMM_WORDS(frames) * sizeof(uint64_t));', '(void)0;')]),
 ('low-memory-unprotected', [('uint64_t s = e->base < PMM_LOW_LIMIT ? PMM_LOW_LIMIT : e->base;', 'uint64_t s = e->base;'), (LINE, '(void)0;')]),
 ('large-protect-rounding', [('(end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)', 'end & ~(PAGE_SIZE - 1)')]),
 ('large-free-inward-rounding', [('s = (s + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);', 's = s & ~(PAGE_SIZE - 1);')]),
 ('large-overflowing-entry', [('if (end < e->base)\n            end = UINT64_MAX;', 'if (end < e->base)\n            continue;')]),
 ('large-ignored-bytes', [('p->ignored_bytes += end - (e->base > limit ? e->base : limit);', 'p->ignored_bytes += end - limit;')]),
 ('large-reservation-pass', [('for (size_t i = 0; t && i < t->count; i++)', 'for (size_t i = 0; t && i + 1 < t->count; i++)')]),
 ('large-usable-count', [('for (uint64_t w = 0; w < PMM_WORDS(frames); w++)\n        p->usable', 'for (uint64_t w = 0; w + 1 < PMM_WORDS(frames); w++)\n        p->usable')]),
 ('large-limit-clip', [('if (end > limit)\n            end = limit;', 'if (end > limit + PAGE_SIZE)\n            end = limit;')]),
 ('error-max-frames', [('frames > PMM_MAX_FRAMES)\n        return PMM_EINVAL;', 'frames > PMM_MAX_FRAMES + 64)\n        return PMM_EINVAL;')]),
 ('error-entry-size', [('bi->mmap->entry_size % 8 != 0', 'bi->mmap->entry_size % 4 != 0')]),
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
        # scope: exactly one bounded clear, against the accepted main pmm.c and the frozen oracle
        man = json.loads((ROOT / MANIFEST).read_text()); prod = (ROOT / PMM).read_text()
        need(prod.count(NEW) == 1 and prod.count(LINE) == 1, 'the bounded clear is present once')
        need(hashlib.sha256(prod.replace(NEW, OLD).encode()).hexdigest() == man['accepted_pmm_c']['sha256'], 'production pmm.c = accepted main pmm.c + exactly the bounded clear')
        frozen = (ROOT / FROZEN).read_text(); ref = json.loads((ROOT / 'tests/reference/pmm-r4m1.json').read_text())
        need(sha(ROOT / FROZEN) == ref['sources_sha256']['pmm-r4m1.c'], 'frozen accepted source')
        k = 'int pmm_alloc_in(struct pmm *p, uint64_t *phys)\n'; acc = prod.replace(NEW, OLD)
        need(frozen[:frozen.index(k)] == acc[:acc.index(k)] and len(acc[:acc.index(k)]) == man['frozen_init_prefix_bytes'], 'frozen helpers and pmm_init_in equal the accepted main')
        report['scope'] = {'accepted_pmm_c_sha256': man['accepted_pmm_c']['sha256'], 'production_pmm_c_sha256': sha(ROOT / PMM), 'changed_statements': 1}
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        root = out / 'root.rs'; rust = out / 'libpmm-host.a'
        root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / GLUE)) + '] mod pmm;\n'
                        'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                        '// host test only: precompiled core references the personality even with panic=abort\n'
                        '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
        run([a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
             '-Cpanic=abort', root, '-o', rust], 'rust-build.txt')
        def regression(tag, flags, src, seeds, timeout=1800):
            r, c, b = out / ('ref-%s.o' % tag), out / ('cand-%s.o' % tag), out / ('reg-%s' % tag)
            if not r.exists(): run(['gcc', *flags, *warn, *renames, '-c', FROZEN, '-o', r], 'ref-%s-build.txt' % tag)
            run(['gcc', *flags, *warn, '-Dboot_mmap_entry=cand_boot_mmap_entry', '-c', src, '-o', c], 'cand-%s-build.txt' % tag)
            run(['gcc', *flags, *warn, HARNESS, r, c, rust, *link, '-o', b], 'reg-%s-build.txt' % tag)
            try: text = run([b, seeds], 'reg-%s.txt' % tag, timeout=timeout, check=False)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b, c
            m = re.search(r'PMM INIT REGRESSION cases=(\d+) checks=(\d+) mismatches=(\d+) negative_faults=(\d)/3 digest=([0-9a-f]{16})', text)
            return text, (dict(cases=int(m[1]), checks=int(m[2]), mismatches=int(m[3]), negative_faults=int(m[4]), digest=m[5]) if m else None), b, c
        report['regression'] = {}; cov = {}
        for tag, flags in (('release', ['-O2']), ('sanitizer', san)):
            text, r, _, _ = regression(tag, flags, PMM, SEEDED)
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
            d = out / 'mutants' / name; d.mkdir(parents=True)
            src = prod
            for old, new in edits:
                need(src.count(old) == 1, 'unique mutation ' + name); src = src.replace(old, new)
            (d / 'pmm.c').write_text(src)
            text, r, b, c = regression('mutant-' + name, ['-O1', '-Iarch/x86_64'], d / 'pmm.c', MUTANT_SEEDED, timeout=600)
            ex = next((x['exit'] for x in report['commands'] if x['log'] == 'reg-mutant-%s.txt' % name), None)
            shutil.rmtree(d)
            for f in (b, c): f.unlink(missing_ok=True)
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
        report['source_sha256'] = {n: sha(ROOT / n) for n in [PMM, FROZEN, MANIFEST, HARNESS, GLUE, 'scripts/test-pmm-init.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:4000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

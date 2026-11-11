#!/usr/bin/env python3
"""R4-M2: Rust pmm_alloc_in, pmm_free_in and pmm_reserve_in against the frozen accepted C (tests/reference/pmm-r4m2.c, the hardened
main). A whole-instance differential (tests/host/pmm-transition-differential.c) runs both on byte-identical guard-paged arenas
(separate, identical and overlapping bitmaps, a bitmap on the instance's fields, the output word on a field or a bitmap word) over
deterministic and seeded allocation/free/reservation/corruption sequences and compares every arena byte after every operation, in a
release build linked with the actual kernel archive and in an ASan/UBSan build linked with an overflow-checked, debug-asserted host
build of the same Rust; digests and coverage must match, with zero mismatches. The memory model is checked statically: the core has
no unsafe code, and the adapter forms no reference, slice or Cell over C memory, and every unsafe block in it is justified. The R4-M1
Rust checks must be byte-identical to main's. Named mutation controls must each be detected. The linked kernel is checked for Rust
ownership, no panic machinery, call-free transitions and entry frames. Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = 'kernel/rust/mm/pmm_transition.rs'; GLUE = 'arch/x86_64/rust/pmm_transition.rs'; M1GLUE = 'arch/x86_64/rust/pmm.rs'; M1CORE = 'kernel/rust/mm/pmm.rs'
REF = 'tests/reference/pmm-r4m2.c'; MANIFEST = 'tests/reference/pmm-r4m2.json'; HARNESS = 'tests/host/pmm-transition-differential.c'
PUBLIC = ('pmm_init_in pmm_alloc_in pmm_free_in pmm_reserve_in pmm_init pmm_alloc_frame pmm_free_frame pmm_reserve_frame pmm_free_count '
          'pmm_kernel pmm_strerror boot_mmap_entry').split()   # the R4-M1 checks are not renamed: both sides use the accepted Rust ones
FUNCS = ('pmm_alloc_in', 'pmm_free_in', 'pmm_reserve_in')
SEQUENCES, MUTANT_SEQUENCES = 600, 300
C_FRAMES = ('pmm_init_in', 'pmm_init', 'pmm_alloc_frame', 'pmm_free_frame', 'pmm_reserve_frame')
# Equivalent and excluded: the order of the free and hint updates after an allocation (distinct fields; no layout the C contract
# defines makes one alias the other); reusing the first-read alloc word instead of re-reading it for the update (nothing is written
# between the two reads); wrapping_mul -> saturating_mul in the reserve range check (differs only for frames >= 2^52, whose storage
# could not exist).
MUTATIONS = [  # (name, file, exact text, replacement): each must make the differential mismatch, fault or abort
 ('alloc-phys-after-state', CORE, '    m.set_phys(0);\n    if m.state() != READY {\n        return ENOTREADY;\n    }\n    let mut w',
  '    if m.state() != READY {\n        return ENOTREADY;\n    }\n    m.set_phys(0);\n    let mut w'),
 ('alloc-ready', CORE, '    if m.state() != READY {\n        return ENOTREADY;\n    }\n    let mut w', '    if m.state() == 0 {\n        return ENOTREADY;\n    }\n    let mut w'),
 ('alloc-hint-ignored', CORE, 'let mut w = m.hint();', 'let mut w = 0;'),
 ('alloc-bound', CORE, 'while w < m.frames() / 64 {', 'while w <= m.frames() / 64 {'),
 ('alloc-free-bits', CORE, 'let free_bits = avail & !alloc;', 'let free_bits = avail;'),
 ('alloc-lowest-bit', CORE, 'free_bits.trailing_zeros()', '(63 - free_bits.leading_zeros())'),
 ('alloc-frame-number', CORE, 'w.wrapping_mul(64).wrapping_add(', 'w.wrapping_mul(63).wrapping_add('),
 ('alloc-free-zero', CORE, 'if m.free() == 0 || frame', 'if m.free() == 1 || frame'),
 ('alloc-low-limit', CORE, 'frame.wrapping_mul(PAGE_SIZE) < LOW_LIMIT', 'frame.wrapping_mul(PAGE_SIZE) <= LOW_LIMIT'),
 ('alloc-frame-bound', CORE, '|| frame >= m.frames() {', '|| frame.wrapping_add(1) >= m.frames() {'),
 ('alloc-corrupt-not-failed', CORE, '            m.set_state(FAILED);\n            return ECORRUPT;\n        }\n        let Some(word)', '            return ECORRUPT;\n        }\n        let Some(word)'),
 ('alloc-set-bit', CORE, 'if m.set_alloc(w, word | 1u64 << (frame % 64)).is_none() {\n            return ECORRUPT;\n        }\n        m.set_free(',
  'if m.set_alloc(w, word | 1u64 << (frame % 63)).is_none() {\n            return ECORRUPT;\n        }\n        m.set_free('),
 ('alloc-free-count', CORE, '        m.set_free(m.free().wrapping_sub(1));\n        m.set_hint(w);', '        m.set_free(m.free());\n        m.set_hint(w);'),
 ('alloc-hint-update', CORE, 'm.set_hint(w);\n        m.set_phys', 'm.set_hint(w.wrapping_add(1));\n        m.set_phys'),
 ('alloc-phys-value', CORE, 'm.set_phys(frame.wrapping_mul(PAGE_SIZE));', 'm.set_phys(frame);'),
 ('alloc-phys-written-first', CORE,
  '        let Some(word) = m.alloc(w) else { return ECORRUPT };\n        if m.set_alloc(w, word | 1u64 << (frame % 64)).is_none() {\n            return ECORRUPT;\n        }\n        m.set_free(m.free().wrapping_sub(1));\n        m.set_hint(w);\n        m.set_phys(frame.wrapping_mul(PAGE_SIZE));',
  '        m.set_phys(frame.wrapping_mul(PAGE_SIZE));\n        let Some(word) = m.alloc(w) else { return ECORRUPT };\n        if m.set_alloc(w, word | 1u64 << (frame % 64)).is_none() {\n            return ECORRUPT;\n        }\n        m.set_free(m.free().wrapping_sub(1));\n        m.set_hint(w);'),
 ('alloc-count-check', CORE, '    if m.free() != 0 {\n        m.set_state(FAILED);', '    if m.free() > 1 {\n        m.set_state(FAILED);'),
 ('alloc-count-not-failed', CORE, '        m.set_state(FAILED);\n        return ECORRUPT; // the count', '        return ECORRUPT; // the count'),
 ('alloc-enomem-hint', CORE, '    m.set_hint(m.frames() / 64);\n    ENOMEM', '    m.set_hint(0);\n    ENOMEM'),
 ('alloc-enomem-hint-kept', CORE, '    m.set_hint(m.frames() / 64);\n    ENOMEM', '    ENOMEM'),
 ('free-bit', CORE, 'word & !(1u64 << (frame % 64))', 'word & !(1u64 << (frame % 63))'),
 ('free-count', CORE, 'm.set_free(m.free().wrapping_add(1));', 'm.set_free(m.free().wrapping_add(2));'),
 ('free-hint-direction', CORE, '    if w < m.hint() {\n        m.set_hint(w);', '    if w > m.hint() {\n        m.set_hint(w);'),
 ('free-hint-kept', CORE, '    if w < m.hint() {\n        m.set_hint(w);\n    }\n', ''),
 ('free-word', CORE, '    let w = frame / 64;\n    let Some(word) = m.alloc(w) else { return ECORRUPT };\n    if m.set_alloc(w, word & !',
  '    let w = frame / 64 + 1;\n    let Some(word) = m.alloc(w) else { return ECORRUPT };\n    if m.set_alloc(w, word & !'),
 ('free-check-skipped', GLUE, '    if err != t::OK {\n        return err;\n    }', '    if err != t::OK && false {\n        return err;\n    }'),
 ('reserve-ready', CORE, '    if m.state() != READY {\n        return ENOTREADY;\n    }\n    if phys % PAGE_SIZE', '    if m.state() == 0 {\n        return ENOTREADY;\n    }\n    if phys % PAGE_SIZE'),
 ('reserve-align', CORE, 'if phys % PAGE_SIZE != 0 {', 'if phys % (PAGE_SIZE / 2) != 0 {'),
 ('reserve-range', CORE, 'if phys >= m.frames().wrapping_mul(PAGE_SIZE) {', 'if phys > m.frames().wrapping_mul(PAGE_SIZE) {'),
 ('reserve-protected', CORE, '        Some(_) => return EPROTECTED,', '        Some(_) => {}'),
 ('reserve-in-use', CORE, '        Some(word) if bit(word, frame) => return EINUSE,', '        Some(word) if bit(word, frame) => {}'),
 ('reserve-precedence', CORE, '    match m.avail(w) {\n        Some(word) if bit(word, frame) => {}\n        Some(_) => return EPROTECTED,\n        None => return ECORRUPT,\n    }\n    match m.alloc(w) {\n        Some(word) if bit(word, frame) => return EINUSE,\n        Some(_) => {}\n        None => return ECORRUPT,\n    }',
  '    match m.alloc(w) {\n        Some(word) if bit(word, frame) => return EINUSE,\n        Some(_) => {}\n        None => return ECORRUPT,\n    }\n    match m.avail(w) {\n        Some(word) if bit(word, frame) => {}\n        Some(_) => return EPROTECTED,\n        None => return ECORRUPT,\n    }'),
 ('reserve-free-zero', CORE, '    if m.free() == 0 {\n        return ECORRUPT;\n    }\n    let Some(word)', '    if m.free() == 1 {\n        return ECORRUPT;\n    }\n    let Some(word)'),
 ('reserve-corrupt-sets-failed', CORE, '    if m.free() == 0 {\n        return ECORRUPT;\n    }\n    let Some(word)', '    if m.free() == 0 {\n        m.set_state(FAILED);\n        return ECORRUPT;\n    }\n    let Some(word)'),
 ('reserve-count', CORE, '    m.set_free(m.free().wrapping_sub(1));\n    OK\n}', '    m.set_free(m.free());\n    OK\n}'),
 ('bit-test', CORE, '(word >> (frame % 64)) & 1 != 0', '(word >> (frame % 32)) & 1 != 0'),
 ('glue-null', GLUE, 'if bitmap.is_null() || !bitmap.is_aligned() || w >= frames / 64 {', 'if !bitmap.is_aligned() || w >= frames / 64 {'),
 ('glue-aligned', GLUE, 'if bitmap.is_null() || !bitmap.is_aligned() || w >= frames / 64 {', 'if bitmap.is_null() || w >= frames / 64 {'),
 ('glue-storage-bound', GLUE, 'if bitmap.is_null() || !bitmap.is_aligned() || w >= frames / 64 {', 'if bitmap.is_null() || !bitmap.is_aligned() || w > frames / 64 {'),
 ('glue-word-offset', GLUE, 'Some(bitmap.wrapping_add(w as usize))', 'Some(bitmap.wrapping_add(w as usize ^ 1))'),
 ('glue-frames-field', GLUE, '    fn frames(&self) -> u64 {\n        // SAFETY: as in `state`.\n        unsafe { addr_of!((*self.p).frames).read() }',
  '    fn frames(&self) -> u64 {\n        // SAFETY: as in `state`.\n        unsafe { addr_of!((*self.p).usable).read() }'),
 ('glue-free-field', GLUE, '        unsafe { addr_of_mut!((*self.p).free).write(v) }', '        unsafe { addr_of_mut!((*self.p).usable).write(v) }'),
 ('glue-hint-field', GLUE, '        unsafe { addr_of!((*self.p).hint).read() }', '        unsafe { addr_of!((*self.p).free).read() }'),
 ('glue-avail-pointer', GLUE, '        let bitmap = unsafe { addr_of!((*self.p).avail).read() };', '        let bitmap = unsafe { addr_of!((*self.p).alloc).read() };'),
 ('glue-alloc-write-target', GLUE, '        let bitmap = unsafe { addr_of!((*self.p).alloc).read() };\n        let word = self.word(bitmap, w)?;\n        // SAFETY: as in `avail`, for one write',
  '        let bitmap = unsafe { addr_of!((*self.p).avail).read() };\n        let word = self.word(bitmap, w)?;\n        // SAFETY: as in `avail`, for one write'),
 ('glue-phys-not-written', GLUE, '        unsafe { self.phys.write(v) }', '        let _ = v;'),
 ('glue-state-not-written', GLUE, '        unsafe { addr_of_mut!((*self.p).state).write(v) }', '        let _ = v;'),
 ('glue-reserve-phys', GLUE, 't::reserve(&Raw { p, phys: core::ptr::null_mut() }, phys)', 't::reserve(&Raw { p, phys: core::ptr::null_mut() }, phys & !0xfff)'),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def frames(obj, names):
    dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', str(obj)], capture_output=True, text=True, check=True).stdout; out = {}
    for f in names:
        i = dis.find('<%s>:\n' % f)
        if i < 0: out[f] = None; continue
        body = dis[i:]; body = body[:body.index('\n\n')] if '\n\n' in body else body
        sub = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', body)
        out[f] = len(re.findall(r'\tpush ', body)) * 8 + (int(sub[1], 16) if sub else 0)
    return out
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True); ap.add_argument('--kernel-dir', type=Path, required=True)
    ap.add_argument('--rustc', required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); archive = a.archive.resolve(); kdir = a.kernel_dir.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build') and archive.is_file() and (kdir / 'vos.bin').is_file(), 'fresh confined output')
    out.mkdir(parents=True)
    report = {'status': 'RUNNING', 'archive_sha256': sha(archive), 'kernel_sha256': sha(kdir / 'vos.bin'), 'commands': []}
    def run(cmd, name, timeout=1800, check=True):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = p.returncode
        if check: need(p.returncode == 0, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        man = json.loads((ROOT / MANIFEST).read_text())
        need(sha(ROOT / REF) == man['sources_sha256']['pmm-r4m2.c'], 'frozen accepted source')
        need(all(sha(ROOT / f) == h for f, h in man['unchanged_sha256'].items()), 'R4-M1 Rust checks unchanged')
        # memory model, statically: no unsafe in the core; no reference, slice or Cell over C memory in the adapter
        core = (ROOT / CORE).read_text(); glue = (ROOT / GLUE).read_text(); code = re.sub(r'//.*', '', glue)
        need('#![forbid(unsafe_code)]' in core and not re.search(r'\bunsafe\b', re.sub(r'//.*', '', core)), 'the core contains no unsafe code')
        need(not re.search(r'&\s*\*|&\s*mut\s*\*|from_raw_parts|\bCell\b|\bas_ref\b|\bas_mut\b|\bderef\b', code), 'the adapter forms no reference, slice or Cell over C memory')
        blocks = list(re.finditer(r'unsafe \{', glue))
        need(all('// SAFETY:' in glue[max(0, b.start() - 400):b.start()] for b in blocks), 'every unsafe block is justified')
        report['unsafe'] = {'core': 0, 'adapter_blocks': len(blocks), 'adapter_entry_points': glue.count('unsafe extern "C" fn'), 'references_to_c_memory': 0}
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        def ref_obj(tag, flags):
            o = out / ('ref-%s.o' % tag)
            run(['gcc', *flags, *warn, *renames, '-c', REF, '-o', o], 'ref-%s-build.txt' % tag)
            return o
        def host_rust(tag, d, lints='-Dwarnings'):
            root = out / ('root-%s.rs' % tag); lib = out / ('librust-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / M1GLUE)) + '] mod pmm;\n#[path=' + json.dumps(str(d / GLUE)) + '] mod pmm_transition;\n'
                            'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n'
                            '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', lints, '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
                 '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib, root
        def differential(tag, flags, ref, rust, seqs, timeout=1800):
            b = out / ('diff-%s' % tag)
            run(['gcc', *flags, *warn, HARNESS, ref, rust, *link, '-o', b], 'diff-%s-build.txt' % tag)
            try: text = run([b, seqs], 'diff-%s.txt' % tag, check=False, timeout=timeout)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b
            m = re.search(r'PMM TRANSITION DIFFERENTIAL ops=(\d+) checks=(\d+) mismatches=(\d+) digest=([0-9a-f]{16})', text)
            return text, (dict(ops=int(m[1]), checks=int(m[2]), mismatches=int(m[3]), digest=m[4]) if m else None), b
        checked, _ = host_rust('checked', ROOT)
        report['differential'] = {}; cov = {}
        for tag, flags, rust in (('release', ['-O2'], archive), ('sanitizer', san, checked)):
            text, r, _ = differential(tag, flags, ref_obj(tag, flags), rust, SEQUENCES)
            need(r and r['mismatches'] == 0 and 'MISMATCH' not in text and 'runtime error' not in text, 'frozen C and Rust transitions identical (%s): %s' % (tag, text[-600:]))
            cov[tag] = re.search(r'^PMM TRANSITION COVERAGE .*$', text, re.M)[0]; report['differential'][tag] = r
        need(report['differential']['release']['digest'] == report['differential']['sanitizer']['digest'] and cov['release'] == cov['sanitizer'], 'transcript independent of build mode')
        c = cov['release']; counts = {k: [int(x) for x in v.split(',')] for k, v in re.findall(r'(alloc|free|reserve|layouts)=([\d,]+)', c)}
        need(all(counts['alloc'][i] > 0 for i in (0, 1, 2, 9)) and all(counts['free'][i] > 0 for i in (0, 1, 3, 4, 5, 6, 9)) and all(counts['reserve'][i] > 0 for i in (0, 1, 3, 4, 5, 9, 10)), 'every result of every transition reached')
        need(all(x > 0 for x in counts['layouts']), 'every memory layout reached')
        named = dict((k, int(v)) for k, v in re.findall(r'(failed|exhaust|reuse|phys_alias|nonmultiple|over_max|refusal|outside_storage)=(\d+)', c))
        need(len(named) == 8 and all(v > 0 for v in named.values()), 'every named class reached')
        report['coverage'] = c
        # named mutation controls (checked host build: overflow, UB-check and panic aborts count as detection)
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 40, 'at least 40 named mutation controls')
        mref = ref_obj('mut', ['-O1'])
        def mutant(m):
            name, rel, old, new = m
            d = out / 'mutants' / name
            for f in (CORE, GLUE): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            src = (d / rel).read_text(); need(src.count(old) == 1, 'unique mutation ' + name); (d / rel).write_text(src.replace(old, new))
            lib, root = host_rust('mutant-' + name, d, '--cap-lints=allow')
            text, r, b = differential('mutant-' + name, ['-O1'], mref, lib, MUTANT_SEQUENCES, timeout=600)
            shutil.rmtree(d)
            for f in (lib, root, b): f.unlink(missing_ok=True)
            return name, r, text
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, len(os.sched_getaffinity(0))))) as pool:
            results = list(pool.map(mutant, MUTATIONS))
        missed = [n for n, r, _ in results if r is not None and r['mismatches'] == 0]
        need(not missed, 'mutations not detected: %s' % missed)
        for name, r, text in results:
            ex = next((x['exit'] for x in report['commands'] if x['log'] == 'diff-mutant-%s.txt' % name), None)
            report['mutations'][name] = {'detected': True, 'mismatches': r['mismatches'] if r else ('hang' if text == 'TIMEOUT' else 'abnormal exit (fault or abort)'), 'exit': ex}
        # linked kernel: ownership, panic freedom, call-free transitions, frames
        k = kdir / 'vos.bin'
        syms = subprocess.run(['nm', k], capture_output=True, text=True, check=True).stdout
        pobj = subprocess.run(['nm', kdir / 'pmm.o'], capture_output=True, text=True, check=True).stdout
        need(all(re.search(r'^[0-9a-f]+ T %s$' % f, syms, re.M) for f in FUNCS), 'kernel defines every transition')
        need(not any(re.search(r' [TtWw] %s$' % f, pobj, re.M) for f in FUNCS) and all(re.search(r' U %s$' % f, pobj, re.M) for f in FUNCS),
             'pmm.o defines none of the transitions and its wrappers call the Rust ones')
        need(not re.search(r'core::panicking|_ZN4core9panicking|panic_fmt|panic_bounds_check', syms), 'no panic machinery in the kernel')
        kmap = (kdir / 'kernel.map').read_text()
        need(all(re.search(r'\.text\.%s\s*\n?\s*0x[0-9a-f]+\s+0x[0-9a-f]+ .*libvos_kernel\.a' % f, kmap) for f in FUNCS), 'link map takes every transition from the Rust archive')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', k], capture_output=True, text=True, check=True).stdout
        report['rust'] = {}
        for f in FUNCS:
            body = dis[dis.index('<%s>:\n' % f):]; body = body[:body.index('\n\n')]
            calls = sorted(set(re.findall(r'call\s+[0-9a-f]+ <([^>]+)>', body)) - {'pmm_free_check_in'})
            need(not calls, 'call-free transition %s: %s' % (f, calls))
            report['rust'][f] = {'frame': frames(k, [f])[f], 'instructions': len(re.findall(r'^\s*[0-9a-f]+:\t(?!int3)', body, re.M))}
        kflags = ['-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx',
                  '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection']
        run(['gcc', '-std=gnu11', *kflags, '-Iarch/x86_64', '-c', REF, '-o', out / 'ref-kernel.o'], 'ref-kernel-build.txt')
        fr = frames(out / 'ref-kernel.o', C_FRAMES + FUNCS); fc = frames(kdir / 'pmm.o', C_FRAMES)
        report['frozen_c_transition_frames'] = {f: fr[f] for f in FUNCS}
        report['c_frames'] = {f: {'frozen': fr[f], 'production': fc[f]} for f in C_FRAMES}
        report['c_frame_growth'] = {f: v for f, v in report['c_frames'].items() if v['frozen'] is not None and v['production'] is not None and v['production'] > v['frozen']}
        report['source_sha256'] = {n: sha(ROOT / n) for n in [CORE, GLUE, M1CORE, M1GLUE, 'arch/x86_64/pmm.c', 'arch/x86_64/pmm.h', 'arch/x86_64/rust/kernel_probe.rs',
                                                              HARNESS, REF, MANIFEST, 'scripts/test-pmm-transition.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:6000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

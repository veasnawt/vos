#!/usr/bin/env python3
"""R4-M1: Rust pmm_free_check_in, pmm_frame_state_in and pmm_check_in against the frozen accepted C (tests/reference/pmm-r4m1.c).
A differential (tests/host/pmm-differential.c) runs the frozen C and the production Rust on the same instances, with every bitmap flush
against PROT_NONE guard pages: deterministic classes (init-built instances, states, boundaries, corruptions, early refusal without
bitmap access, instances outside the documented bounds), then seeded instances. It runs in a release build linked with the actual
kernel archive and in an ASan/UBSan build linked with an overflow-checked, debug-asserted host build of the same Rust; the transcript
digests and the exact coverage must match, with zero mismatches, the struct pmm ABI confirmed and no instance or bitmap changed.
Named mutation controls must each be detected. The linked kernel is checked for Rust ownership, no panic machinery, call-free checks
and entry frames, and the core for the absence of unsafe code. Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = 'kernel/rust/mm/pmm.rs'; GLUE = 'arch/x86_64/rust/pmm.rs'
PUBLIC = ('pmm_init_in pmm_alloc_in pmm_free_check_in pmm_free_in pmm_reserve_in pmm_frame_state_in pmm_check_in pmm_init pmm_alloc_frame '
          'pmm_free_frame pmm_reserve_frame pmm_free_count pmm_kernel pmm_strerror').split()
FUNCS = ('pmm_free_check_in', 'pmm_frame_state_in', 'pmm_check_in')
SEEDED, MUTANT_SEEDED = 20000, 4000
ABI = 'PMM ABI struct=64 avail=0 alloc=8 frames=16 usable=24 free=32 hint=40 ignored=48 state=56'
COVERAGE = 'PMM COVERAGE free=@@ state=@@ check=@@'
DIVERGENCE = 'PMM DOCUMENTED DIVERGENCE outside-contract instances with real buffers: @@'
# Equivalent and excluded: `free > usable` dropped (a recount never has more free than usable frames); the zero-frames bound
# (`frames - 1 >= MAX_FRAMES` with frames == 0) alone dropped (with zero frames every check decides before asking for the bitmaps); the core's slice-length check dropped
# (the adapter forms exactly frames / 64 words); frame_state's `frame >= frames` weakened or dropped (the bounds-checked word
# read already answers FRAME_UNAVAILABLE, without access, for every frame at or above frames).
MUTATIONS = [  # (name, file, exact text, replacement): each must make the differential mismatch, fault or abort
 ('free-ready', CORE, 'if p.state() != READY {\n        return ENOTREADY;', 'if p.state() == 0 {\n        return ENOTREADY;'),
 ('free-align', CORE, 'phys % PAGE_SIZE != 0', 'phys % (PAGE_SIZE / 2) != 0'),
 ('free-range', CORE, 'phys >= p.frames().wrapping_mul(PAGE_SIZE)', 'phys > p.frames().wrapping_mul(PAGE_SIZE)'),
 ('free-range-saturating', CORE, 'phys >= p.frames().wrapping_mul(PAGE_SIZE)', 'phys >= p.frames().saturating_mul(PAGE_SIZE)'),
 ('free-early-bitmap-access', CORE, '        return ENOTREADY;\n    }\n',
  '        return ENOTREADY;\n    }\n    if let Some(b) = p.bitmaps() {\n        if bit(b.avail, phys / PAGE_SIZE) == Some(false) {\n            return EPROTECTED;\n        }\n    }\n'),
 ('free-protected-code', CORE, 'Some(false) => return EPROTECTED,', 'Some(false) => return ENOTALLOC,'),
 ('free-notalloc-code', CORE, 'Some(false) => return ENOTALLOC,', 'Some(false) => return EPROTECTED,'),
 ('free-allocated-first', CORE, 'match bit(b.avail, frame) {\n        Some(true) => {}\n        Some(false) => return EPROTECTED,',
  'match bit(b.alloc, frame) {\n        Some(true) => {}\n        Some(false) => return EPROTECTED,'),
 ('free-counters', CORE, 'p.free() >= p.usable()', 'p.free() > p.usable()'),
 ('free-counters-dropped', CORE, 'if p.free() >= p.usable() {', 'if p.free() >= p.usable() && false {'),
 ('bit-index', CORE, '(x >> (frame % 64)) & 1', '(x >> (frame % 32)) & 1'),
 ('bit-word', CORE, 'usize::try_from(frame / 64)', 'usize::try_from(frame / 64 + 1)'),
 ('state-ready', CORE, 'if p.state() != READY || frame', 'if p.state() == 0 || frame'),
 ('state-early-bitmap-access', CORE, '    let frame = phys / PAGE_SIZE;\n    if p.state() != READY || frame',
  '    let frame = phys / PAGE_SIZE;\n    if p.state() != READY && p.bitmaps().is_some_and(|b| bit(b.avail, frame) == Some(true)) {\n        return FRAME_FREE;\n    }\n    if p.state() != READY || frame'),
 ('state-swap', CORE, 'Some(true) => FRAME_ALLOCATED,\n        Some(false) => FRAME_FREE,', 'Some(true) => FRAME_FREE,\n        Some(false) => FRAME_ALLOCATED,'),
 ('state-ignores-avail', CORE, 'if bit(b.avail, frame) != Some(true) {', 'if bit(b.avail, frame).is_none() {'),
 ('state-wrong-bitmap', CORE, 'match bit(b.alloc, frame) {\n        Some(true) => FRAME_ALLOCATED', 'match bit(b.avail, frame) {\n        Some(true) => FRAME_ALLOCATED'),
 ('check-ready', CORE, 'if p.state() != READY {\n        return ECORRUPT;', 'if p.state() == 0 {\n        return ECORRUPT;'),
 ('check-alloc-outside-avail', CORE, 'if alloc & !avail != 0 {', 'if false {'),
 ('check-low-limit-short', CORE, 'w < LOW_LIMIT / PAGE_SIZE / 64 && avail', 'w < LOW_LIMIT / PAGE_SIZE / 64 - 1 && avail'),
 ('check-low-limit-long', CORE, 'w < LOW_LIMIT / PAGE_SIZE / 64 && avail', 'w <= LOW_LIMIT / PAGE_SIZE / 64 && avail'),
 ('check-hint-inclusive', CORE, 'w < p.hint() && m != 0', 'w <= p.hint() && m != 0'),
 ('check-hint-dropped', CORE, 'w < p.hint() && m != 0', 'w < p.hint() && m != 0 && false'),
 ('check-usable-dropped', CORE, 'if usable != p.usable() || free', 'if free'),
 ('check-free-dropped', CORE, '|| free != p.free() ||', '||'),
 ('check-count-bits', CORE, 'while x != 0 {', 'while x > 1 {'),
 ('check-free-mask', CORE, 'let m = avail & !alloc;', 'let m = avail;'),
 ('check-empty-shortcut', CORE, 'if words != 0 {', 'if true {'),
 ('bounds-multiple', CORE, ' | (frames % 64 != 0)', ''),
 ('bounds-max', CORE, '(frames.wrapping_sub(1) >= MAX_FRAMES)', '(frames == 0)'),
 ('bounds-null-avail', CORE, ' | (avail == 0)', ''),
 ('bounds-null-alloc', CORE, ' | (alloc == 0)', ''),
 ('bounds-aligned', CORE, ' | ((avail | alloc) % 8 != 0)', ''),
 ('bounds-aligned-avail-only', CORE, '((avail | alloc) % 8 != 0)', '(avail % 8 != 0)'),
 ('glue-bounds-arguments', GLUE, 'm::instance_ok(self.frames, self.avail.addr(), self.alloc.addr())', 'm::instance_ok(self.frames, self.avail.addr(), self.avail.addr())'),
 ('glue-words-long', GLUE, 'let words = (self.frames / 64) as usize;', 'let words = (self.frames / 64 + 1) as usize;'),
 ('glue-words-short', GLUE, 'let words = (self.frames / 64) as usize;', 'let words = (self.frames / 128) as usize;'),
 ('glue-usable-field', GLUE, 'fn usable(&self) -> u64 { self.usable }', 'fn usable(&self) -> u64 { self.free }'),
 ('glue-hint-field', GLUE, 'fn hint(&self) -> u64 { self.hint }', 'fn hint(&self) -> u64 { 0 }'),
 ('glue-state-field', GLUE, 'fn state(&self) -> i32 { self.state }', 'fn state(&self) -> i32 { m::READY }'),
 ('glue-frames-field', GLUE, 'fn frames(&self) -> u64 { self.frames }', 'fn frames(&self) -> u64 { self.frames & !63 }'),
 ('glue-bitmaps-swapped', GLUE, 'avail: core::slice::from_raw_parts(self.avail, words), alloc: core::slice::from_raw_parts(self.alloc, words)',
  'avail: core::slice::from_raw_parts(self.alloc, words), alloc: core::slice::from_raw_parts(self.avail, words)'),
]
C_FRAMES = ('pmm_init_in', 'pmm_alloc_in', 'pmm_free_in', 'pmm_reserve_in', 'pmm_init', 'pmm_alloc_frame', 'pmm_free_frame', 'pmm_reserve_frame')
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
    def run(cmd, name, expect=0, timeout=1800, check=True):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = p.returncode
        if check: need(p.returncode == expect, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        ref = json.loads((ROOT / 'tests/reference/pmm-r4m1.json').read_text())
        for n, v in ref['sources_sha256'].items(): need(sha(ROOT / 'tests/reference' / n) == v, 'frozen accepted source ' + n)
        # the safe core has no unsafe code; the adapter's unsafe is exactly the struct reads and the slice formation, each justified
        core = (ROOT / CORE).read_text(); glue = (ROOT / GLUE).read_text()
        need('#![forbid(unsafe_code)]' in core and not re.search(r'\bunsafe\b', re.sub(r'//.*', '', core)), 'the core contains no unsafe code')
        blocks = re.findall(r'unsafe \{', glue)
        need(len(blocks) == 4 and glue.count('unsafe extern "C" fn') == 3, 'the adapter has exactly 4 unsafe blocks and 3 unsafe C entry points')
        need(all('// SAFETY:' in glue[max(0, m.start() - 900):m.start()] for m in re.finditer(r'unsafe \{', glue)), 'every unsafe block is justified')
        report['unsafe'] = {'core': 0, 'adapter_blocks': len(blocks), 'adapter_entry_points': 3}
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64', '-Itests/host']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        # the frozen init's boot_mmap_entry is the harness's (the kernel's is Rust since B1): only the three checks come from Rust
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC + ['boot_mmap_entry']]
        def ref_obj(tag, flags):
            o = out / ('ref-%s.o' % tag)
            run(['gcc', *flags, *warn, *renames, '-c', 'tests/reference/pmm-r4m1.c', '-o', o], 'ref-%s-build.txt' % tag)
            return o
        def host_rust(tag, d, lints='-Dwarnings'):
            root = out / ('root-%s.rs' % tag); lib = out / ('librust-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(d / GLUE)) + '] mod pmm;\n'
                            'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n'
                            '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', lints, '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
                 '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib, root
        def differential(tag, flags, objs, rust, args=(), timeout=1800):
            b = out / ('diff-%s' % tag)
            run(['gcc', *flags, *warn, 'tests/host/pmm-differential.c', *objs, rust, *link, '-o', b], 'diff-%s-build.txt' % tag)
            try: text = run([b, *map(str, args)], 'diff-%s.txt' % tag, check=False, timeout=timeout)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b
            m = re.search(r'PMM DIFFERENTIAL instances=(\d+) probes=(\d+) checks=(\d+) mismatches=(\d+) digest=([0-9a-f]{16})', text)
            return text, (dict(instances=int(m[1]), probes=int(m[2]), checks=int(m[3]), mismatches=int(m[4]), digest=m[5]) if m else None), b
        checked, _ = host_rust('checked', ROOT)
        report['differential'] = {}; lines = {}
        for tag, flags, rust in (('release', ['-O2'], archive), ('sanitizer', san, checked)):
            text, r, _ = differential(tag, flags, [ref_obj(tag, flags)], rust, (SEEDED,))
            need(r and r['mismatches'] == 0 and 'MISMATCH' not in text and 'runtime error' not in text, 'frozen C and Rust identical (%s): %s' % (tag, text[-600:]))
            need(ABI in text, 'struct pmm ABI (%s)' % tag)
            cov = re.search(r'^PMM COVERAGE .*$', text, re.M)[0]; div = re.search(r'^PMM DOCUMENTED DIVERGENCE .*$', text, re.M)[0]
            need(re.fullmatch(COVERAGE.replace('@@', r'\S+') + r' .*', cov) and re.fullmatch(re.escape(DIVERGENCE).replace('@@', '.*'), div), 'coverage lines (%s)' % tag)
            lines[tag] = (cov, div); report['differential'][tag] = r
        need(report['differential']['release']['digest'] == report['differential']['sanitizer']['digest'] and lines['release'] == lines['sanitizer'],
             'transcript and coverage independent of build mode')
        cov = lines['release'][0]
        counts = {k: [int(x) for x in v.split(',')] for k, v in re.findall(r'(free|state|check)=([\d,]+)', cov)}
        # every return value of every check is reached: free OK..ECORRUPT except the codes it never returns, all frame states, check OK/ECORRUPT
        need(all(counts['free'][i] > 0 for i in (0, 1, 3, 4, 5, 6, 9)) and all(counts['free'][i] == 0 for i in (2, 7, 8, 10)), 'free_check result coverage')
        need(all(c > 0 for c in counts['state']) and counts['check'][0] > 0 and counts['check'][9] > 0 and sum(counts['check']) == counts['check'][0] + counts['check'][9],
             'frame_state and check result coverage')
        named = dict(re.findall(r'(early|outside|outside_c_defined|lowmem|wordedge|hint|counter|alloc_outside_avail|exhaustive)=(\d+)', cov))
        need(all(int(v) > 0 for v in named.values()) and len(named) == 9, 'every deterministic class reached')
        report['coverage'] = cov; report['documented_divergence'] = lines['release'][1]
        # named mutation controls (checked host build, so overflow, UB-check and panic aborts count as detection)
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 35, 'at least 35 named mutation controls')
        mref = ref_obj('mut', ['-O1'])
        def mutant(m):
            name, rel, old, new = m
            d = out / 'mutants' / name
            for f in (CORE, GLUE): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            src = (d / rel).read_text(); need(src.count(old) == 1, 'unique mutation ' + name); (d / rel).write_text(src.replace(old, new))
            lib, root = host_rust('mutant-' + name, d, '--cap-lints=allow')   # a mutant may leave a constant unused
            text, r, b = differential('mutant-' + name, ['-O1'], [mref], lib, (MUTANT_SEEDED,), timeout=600)
            shutil.rmtree(d)
            for f in (lib, root, b): f.unlink(missing_ok=True)
            return name, r, text
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, len(os.sched_getaffinity(0))))) as pool:
            results = list(pool.map(mutant, MUTATIONS))
        missed = [name for name, r, _ in results if r is not None and r['mismatches'] == 0]
        need(not missed, 'mutations not detected: %s' % missed)
        for name, r, text in results:
            ex = next((c['exit'] for c in report['commands'] if c['log'] == 'diff-mutant-%s.txt' % name), None)
            report['mutations'][name] = {'detected': True, 'mismatches': r['mismatches'] if r else ('hang' if text == 'TIMEOUT' else 'abnormal exit (fault or abort)'), 'exit': ex}
        # linked kernel: ownership, panic freedom, call-free checks, frames
        k = kdir / 'vos.bin'
        syms = subprocess.run(['nm', k], capture_output=True, text=True, check=True).stdout
        pobj = subprocess.run(['nm', kdir / 'pmm.o'], capture_output=True, text=True, check=True).stdout
        need(all(re.search(r'^[0-9a-f]+ T %s$' % f, syms, re.M) for f in FUNCS), 'kernel defines every check')
        need(not any(re.search(r' [TtWw] %s$' % f, pobj, re.M) for f in FUNCS) and all(re.search(r' U %s$' % f, pobj, re.M) for f in ('pmm_free_check_in',)),
             'pmm.o defines none of the checks (Rust owns them) and calls the Rust free check')
        need(not re.search(r'core::panicking|_ZN4core9panicking|panic_fmt|panic_bounds_check', syms), 'no panic machinery in the kernel')
        kmap = (kdir / 'kernel.map').read_text()
        need(all(re.search(r'\.text\.%s\s*\n?\s*0x[0-9a-f]+\s+0x[0-9a-f]+ .*libvos_kernel\.a' % f, kmap) for f in FUNCS), 'link map takes every check from the Rust archive')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', k], capture_output=True, text=True, check=True).stdout
        report['rust'] = {}
        for f in FUNCS:
            body = dis[dis.index('<%s>:\n' % f):]; body = body[:body.index('\n\n')]
            calls = sorted(set(re.findall(r'call\s+[0-9a-f]+ <([^>]+)>', body)))
            need(not calls, 'call-free check %s: %s' % (f, calls))
            report['rust'][f] = {'frame': frames(k, [f])[f], 'instructions': len(re.findall(r'^\s*[0-9a-f]+:\t(?!int3)', body, re.M))}
        # C entry frames: production pmm.o in the kernel vs the frozen C compiled with the kernel's flags
        kflags = ['-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx',
                  '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection']
        run(['gcc', '-std=gnu11', *kflags, '-Iarch/x86_64', '-c', 'tests/reference/pmm-r4m1.c', '-o', out / 'ref-kernel.o'], 'ref-kernel-build.txt')
        fr = frames(out / 'ref-kernel.o', C_FRAMES + FUNCS); fc = frames(kdir / 'pmm.o', C_FRAMES)
        report['frozen_c_check_frames'] = {f: fr[f] for f in FUNCS}
        report['c_frames'] = {f: {'frozen': fr[f], 'production': fc[f]} for f in C_FRAMES}
        report['c_frame_growth'] = {f: v for f, v in report['c_frames'].items() if v['frozen'] is not None and v['production'] is not None and v['production'] > v['frozen']}
        report['source_sha256'] = {n: sha(ROOT / n) for n in [CORE, GLUE, 'arch/x86_64/pmm.c', 'arch/x86_64/pmm.h', 'arch/x86_64/rust/kernel_probe.rs', 'tests/host/pmm-differential.c',
                                                              'tests/reference/pmm-r4m1.c', 'tests/reference/pmm-r4m1.json', 'scripts/test-pmm-rust.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:6000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

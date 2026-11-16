#!/usr/bin/env python3
"""R4-M3: Rust pmm_init_in against the frozen accepted C (tests/reference/pmm-r4m3.c, the complete pmm.c of main). Scope:
production pmm.c must equal the frozen file byte for byte once the note that replaces pmm_init_in and its four helpers is undone.
The memory model is checked statically: the core has no unsafe code, and the adapter forms no reference, slice or Cell over C
memory, justifies every unsafe block and uses byte offsets that are compiled against the C headers. A whole-state differential
(tests/host/pmm-init-differential.c) runs both on the same guard-paged memory in eight layouts (separate, identical and
overlapping bitmaps, a bitmap on the instance's counters, the map or the reservation table inside a bitmap, the instance inside
its inputs) over valid, malformed, overlapping and overflowing memory maps and reservation tables, and compares the result and
every byte; five classes where the C is undefined run the Rust alone and require its documented refusal without a stray write.
It runs in a release build linked with the actual kernel archive and in an ASan/UBSan build linked with an overflow-checked host
build of the same Rust; digests and coverage must match, with zero mismatches. Named mutation controls must each be detected.
The linked kernel is checked for Rust ownership, no panic machinery, the callee set and entry frames. The accepted R4-M1 and
R4-M2 Rust must be byte-identical to main's. Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = 'kernel/rust/mm/pmm_init.rs'; GLUE = 'arch/x86_64/rust/pmm_init.rs'; PMM = 'arch/x86_64/pmm.c'
M1GLUE = 'arch/x86_64/rust/pmm.rs'; M2GLUE = 'arch/x86_64/rust/pmm_transition.rs'
REF = 'tests/reference/pmm-r4m3.c'; MANIFEST = 'tests/reference/pmm-r4m3.json'; HARNESS = 'tests/host/pmm-init-differential.c'
PUBLIC = 'pmm_init_in pmm_init pmm_alloc_frame pmm_free_frame pmm_reserve_frame pmm_free_count pmm_kernel pmm_strerror boot_mmap_entry'.split()
HELPERS = ('count_bits', 'set_frames', 'clear_frames', 'protect')
NOTE = ('/*\n * R4-M3: pmm_init_in and its helpers (pmm.h ABI unchanged) are Rust: kernel/rust/mm/pmm_init.rs behind\n'
        ' * arch/x86_64/rust/pmm_init.rs. The accepted C is tests/reference/pmm-r4m3.c.\n */\n\n')
SEEDED, MUTANT_SEEDED = 20000, 2500
C_FRAMES = ('pmm_init', 'pmm_alloc_frame', 'pmm_free_frame', 'pmm_reserve_frame')
# The adapter's byte offsets, each bound to the C headers by a generated translation unit.
OFFSETS = {'PMM_SIZE': 'sizeof(struct pmm)', 'PMM_AVAIL': 'offsetof(struct pmm, avail)', 'PMM_ALLOC': 'offsetof(struct pmm, alloc)', 'PMM_FRAMES': 'offsetof(struct pmm, frames)',
           'PMM_USABLE': 'offsetof(struct pmm, usable)', 'PMM_FREE': 'offsetof(struct pmm, free)', 'PMM_HINT': 'offsetof(struct pmm, hint)', 'PMM_IGNORED': 'offsetof(struct pmm, ignored_bytes)',
           'PMM_STATE': 'offsetof(struct pmm, state)', 'BI_MMAP': 'offsetof(struct boot_info, mmap)', 'BI_MMAP_ENTRIES': 'offsetof(struct boot_info, mmap_entries)',
           'TAG_ENTRY_SIZE': 'offsetof(struct mb2_tag_mmap, entry_size)', 'TAG_ENTRY_VERSION': 'offsetof(struct mb2_tag_mmap, entry_version)', 'ENTRY_BASE': 'offsetof(struct mb2_mmap_entry, base)',
           'ENTRY_LENGTH': 'offsetof(struct mb2_mmap_entry, length)', 'ENTRY_TYPE': 'offsetof(struct mb2_mmap_entry, type)', 'RESV_MAX': 'RESV_MAX', 'RANGE_SIZE': 'sizeof(struct phys_range)',
           'RANGE_END': 'offsetof(struct phys_range, end)', 'RESV_COUNT': 'offsetof(struct resv_table, count)'}
CONSTANTS = {'PAGE_SIZE': 'PAGE_SIZE', 'LOW_LIMIT': 'PMM_LOW_LIMIT', 'MAX_FRAMES': 'PMM_MAX_FRAMES', 'AVAILABLE': 'MB2_MMAP_AVAILABLE', 'ENTRY_MIN': 'sizeof(struct mb2_mmap_entry)', 'READY': 'PMM_READY',
             'FAILED': 'PMM_FAILED', 'OK': 'PMM_OK', 'EMMAP': 'PMM_EMMAP', 'EINVAL': 'PMM_EINVAL', 'ECORRUPT': 'PMM_ECORRUPT'}
# Equivalent and excluded, because unobservable at pmm_init_in's boundary:
# - the free pass's 1 MiB clamp alone, or the low-memory clear alone, dropped: each covers the other (both together is a control);
# - an empty available entry not skipped: its range is empty either way and it adds zero ignored bytes;
# - `>=` for `>` where the ignored bytes are counted, and where a protected end is clipped: at equality both sides give the same value;
# - not rounding a protected or reserved start down before the frame division (except where it decides emptiness, a control);
# - dropping protect's `start >= limit` test: the end is clipped to the limit, so such a range is empty either way;
# - reusing the first read of an entry's base after ignored_bytes is updated: where they alias, the value only feeds a range that
#   is empty either way;
# - the adapter's own storage and NULL checks on a bitmap word: the unmutated core never asks outside them (paired with a core
#   mutation, the storage bound is a control).
KERNEL = '        if m.kind(e) == AVAILABLE || length == 0 {\n            continue;\n        }\n'
CLAMP = 'let mut s = if base < LOW_LIMIT { LOW_LIMIT } else { base };'
LOWCLEAR = '    if change_frames(m, 0, low / PAGE_SIZE, false).is_none() {\n        return ECORRUPT;\n    }\n'
ZERO_AVAIL = '    while w < words {\n        if m.set_avail(w, 0).is_none() {'; ZERO_ALLOC = '    while w < words {\n        if m.set_alloc(w, 0).is_none() {'
WORD = 'if bitmap.is_null() || !bitmap.is_aligned() || w >= self.frames / 64 {'
def offset(name, old, new): return (GLUE, 'const %s: usize = %d;' % (name, old), 'const %s: usize = %d;' % (name, new))
MUTATIONS = [  # (name, [(file, exact text, replacement), ...]): each must make the differential mismatch, fault or abort
 ('instance-not-zeroed', [(CORE, '    m.zero_instance();\n', '')]),
 ('state-not-failed-first', [(CORE, '    m.set_state(FAILED);\n    if bitmaps_unusable', '    if bitmaps_unusable')]),
 ('einval-bitmaps-ignored', [(CORE, 'if bitmaps_unusable || frames == 0', 'if frames == 0')]),
 ('einval-frames-zero', [(CORE, 'bitmaps_unusable || frames == 0 || frames % 64', 'bitmaps_unusable || frames % 64')]),
 ('einval-frames-multiple', [(CORE, 'frames % 64 != 0 || frames > MAX_FRAMES', 'frames % 32 != 0 || frames > MAX_FRAMES')]),
 ('einval-frames-max', [(CORE, 'frames > MAX_FRAMES {', 'frames > MAX_FRAMES + 64 {')]),
 ('emmap-no-info-ignored', [(CORE, 'if no_info || !m.mmap_present()', 'if !m.mmap_present()')]),
 ('emmap-entries-zero', [(CORE, ' || m.mmap_entries() == 0 {\n        return EMMAP;', ' {\n        return EMMAP;')]),
 ('emmap-version', [(CORE, 'if m.entry_version() != Some(0) {', 'if m.entry_version() == None {')]),
 ('emmap-entry-size-minimum', [(CORE, 'Some(size) if size >= ENTRY_MIN && size % 8 == 0', 'Some(size) if size >= 16 && size % 8 == 0')]),
 ('emmap-entry-size-multiple', [(CORE, 'Some(size) if size >= ENTRY_MIN && size % 8 == 0', 'Some(size) if size >= ENTRY_MIN && size % 4 == 0')]),
 ('bitmap-pointers-not-stored', [(CORE, '    m.set_bitmaps();\n', '')]),
 ('frames-not-stored', [(CORE, '    m.set_frames(frames);\n', '')]),
 ('zero-avail-short', [(CORE, ZERO_AVAIL, ZERO_AVAIL.replace('while w < words', 'while w.wrapping_add(1) < words'))]),
 ('zero-alloc-short', [(CORE, ZERO_ALLOC, ZERO_ALLOC.replace('while w < words', 'while w.wrapping_add(1) < words'))]),
 ('zero-alloc-dropped', [(CORE, ZERO_ALLOC, ZERO_ALLOC.replace('while w < words', 'while w < 0'))]),
 ('free-type', [(CORE, 'if m.kind(e) != AVAILABLE || length == 0 || end < base {', 'if m.kind(e) == 2 || length == 0 || end < base {')]),
 ('free-overflow-accepted', [(CORE, 'if m.kind(e) != AVAILABLE || length == 0 || end < base {', 'if m.kind(e) != AVAILABLE || length == 0 {')]),
 ('ignored-from-limit', [(CORE, 'let from = if base > limit { base } else { limit };', 'let from = limit;')]),
 ('ignored-dropped', [(CORE, 'm.set_ignored(m.ignored().wrapping_add(end.wrapping_sub(from)));', 'let _ = from;')]),
 ('ignored-not-accumulated', [(CORE, 'm.set_ignored(m.ignored().wrapping_add(end.wrapping_sub(from)));', 'm.set_ignored(end.wrapping_sub(from));')]),
 ('free-limit-clip', [(CORE, '        if end > limit {\n            end = limit;\n        }\n', '')]),
 ('free-inward-start', [(CORE, 's = s.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1); // inward', 's &= !(PAGE_SIZE - 1); // inward')]),
 ('free-inward-end', [(CORE, '        end &= !(PAGE_SIZE - 1);\n', '        end = end.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1);\n')]),
 ('free-clears-instead', [(CORE, 'change_frames(m, s / PAGE_SIZE, end / PAGE_SIZE, true)', 'change_frames(m, s / PAGE_SIZE, end / PAGE_SIZE, false)')]),
 ('free-frame-number', [(CORE, 'change_frames(m, s / PAGE_SIZE, end / PAGE_SIZE, true)', 'change_frames(m, s / PAGE_SIZE + 1, end / PAGE_SIZE, true)')]),
 ('protect-applies-to-ram', [(CORE, KERNEL, KERNEL.replace('m.kind(e) == AVAILABLE || ', ''))]),
 ('protect-pass-dropped', [(CORE, KERNEL, KERNEL.replace('m.kind(e) == AVAILABLE || length == 0', 'length != u64::MAX'))]),
 ('protect-overflow-skipped', [(CORE, '        if end < base {\n            end = u64::MAX; // overflowing: extends to 2^64\n        }\n', '        if end < base {\n            continue;\n        }\n')]),
 ('protect-end-rounding', [(CORE, 'let e = if end >= limit { limit } else { end.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1) };', 'let e = if end >= limit { limit } else { end & !(PAGE_SIZE - 1) };')]),
 ('protect-not-clipped', [(CORE, 'let e = if end >= limit { limit } else { end.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1) };', 'let e = end.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1);')]),
 ('protect-sets-instead', [(CORE, 'change_frames(m, start / PAGE_SIZE, e / PAGE_SIZE, false)', 'change_frames(m, start / PAGE_SIZE, e / PAGE_SIZE, true)')]),
 ('protect-instance-check-dropped', [(CORE, '    if !m.instance_avail_is_bitmap() {\n        return None; // the C would clear through whatever `p->avail` now holds\n    }\n', '')]),
 ('reservations-dropped', [(CORE, '        if r >= count {\n            break;', '        if r >= count || count != u64::MAX {\n            break;')]),
 ('reservations-short', [(CORE, '        if r >= count {\n            break;', '        if r.wrapping_add(1) >= count {\n            break;')]),
 ('reservation-empty-unaligned', [(CORE, 'protect(m, start & !(PAGE_SIZE - 1), end, limit)', 'protect(m, start, end, limit)')]),
 ('low-memory-unprotected', [(CORE, CLAMP, 'let mut s = base;'), (CORE, LOWCLEAR, '    let _ = low;\n')]),
 ('low-clear-unbounded', [(CORE, 'let low = if limit < LOW_LIMIT { limit } else { LOW_LIMIT };', 'let low = LOW_LIMIT;')]),
 ('low-clear-max-not-min', [(CORE, 'let low = if limit < LOW_LIMIT { limit } else { LOW_LIMIT };', 'let low = if limit > LOW_LIMIT { limit } else { LOW_LIMIT };')]),
 ('low-clear-sets', [(CORE, 'change_frames(m, 0, low / PAGE_SIZE, false)', 'change_frames(m, 0, low / PAGE_SIZE, true)')]),
 ('count-short', [(CORE, '    while w < words {\n        let Some(word) = m.avail(w)', '    while w.wrapping_add(1) < words {\n        let Some(word) = m.avail(w)')]),
 ('count-bits', [(CORE, 'x &= x.wrapping_sub(1);', 'x >>= 1;')]),
 ('count-not-accumulated', [(CORE, 'm.set_usable(m.usable().wrapping_add(count_bits(word)));', 'm.set_usable(count_bits(word));')]),
 ('free-count', [(CORE, '    m.set_free(m.usable());\n', '    m.set_free(0);\n')]),
 ('hint', [(CORE, '    m.set_hint(0);\n', '    m.set_hint(1);\n')]),
 ('state-not-ready', [(CORE, '    m.set_state(READY);\n    OK', '    OK')]),
 ('run-mask-low', [(CORE, 'let below = if from >= 64 { !0 } else { (1u64 << (from % 64)).wrapping_sub(1) };', 'let below = 0;')]),
 ('run-mask-high', [(CORE, 'let upto = if end.wrapping_sub(low) >= 64 { !0 }', 'let upto = if end.wrapping_sub(low) > 64 { !0 }')]),
 ('run-word-loop', [(CORE, 'while w.wrapping_mul(64) < end {', 'while w.wrapping_mul(64) <= end {')]),
 ('storage-bound-with-word-loop', [(CORE, 'while w.wrapping_mul(64) < end {', 'while w.wrapping_mul(64) <= end {'), (GLUE, WORD, WORD.replace('w >= self.frames / 64', 'w > self.frames / 64'))]),
 ('glue-zero-instance-short', [(GLUE, 'while at < PMM_SIZE {', 'while at < PMM_SIZE - 8 {')]),
 ('glue-state-offset', [offset('PMM_STATE', 56, 48)]), ('glue-ignored-offset', [offset('PMM_IGNORED', 48, 40)]), ('glue-usable-offset', [offset('PMM_USABLE', 24, 32)]),
 ('glue-free-offset', [offset('PMM_FREE', 32, 40)]), ('glue-hint-offset', [offset('PMM_HINT', 40, 32)]), ('glue-frames-offset', [offset('PMM_FRAMES', 16, 24)]),
 ('glue-bitmap-pointers-swapped', [(GLUE, 'self.field::<*mut u64>(PMM_AVAIL).write(self.avail);\n            self.field::<*mut u64>(PMM_ALLOC).write(self.alloc);',
                                    'self.field::<*mut u64>(PMM_AVAIL).write(self.alloc);\n            self.field::<*mut u64>(PMM_ALLOC).write(self.avail);')]),
 ('glue-alloc-zeroes-avail', [(GLUE, '        let word = self.word(self.alloc, w)?;', '        let word = self.word(self.avail, w)?;')]),
 ('glue-instance-check-inverted', [(GLUE, 'self.field::<*mut u64>(PMM_AVAIL).read() == self.avail', 'self.field::<*mut u64>(PMM_AVAIL).read() != self.avail')]),
 ('glue-mmap-offset', [offset('BI_MMAP', 32, 24)]), ('glue-entries-offset', [offset('BI_MMAP_ENTRIES', 40, 44)]),
 ('glue-tag-fields-swapped', [offset('TAG_ENTRY_SIZE', 8, 12), offset('TAG_ENTRY_VERSION', 12, 8)]),
 ('glue-entry-length-offset', [offset('ENTRY_LENGTH', 8, 0)]), ('glue-entry-type-offset', [offset('ENTRY_TYPE', 16, 20)]), ('glue-entry-base-offset', [offset('ENTRY_BASE', 0, 8)]),
 ('glue-range-end-offset', [offset('RANGE_END', 8, 16)]), ('glue-range-size', [offset('RANGE_SIZE', 24, 16)]), ('glue-resv-count-offset', [offset('RESV_COUNT', 1920, 1912)]),
 ('glue-resv-bound-dropped', [(GLUE, 'if self.t.is_null() || i >= RESV_MAX {', 'if self.t.is_null() {')]),
 ('glue-null-instance-accepted', [(GLUE, 'if p.is_null() || !p.cast::<u64>().is_aligned() {', 'if !p.cast::<u64>().is_aligned() {')]),
 ('glue-misaligned-instance-accepted', [(GLUE, 'if p.is_null() || !p.cast::<u64>().is_aligned() {', 'if p.is_null() {')]),
 ('glue-misaligned-bitmap-accepted', [(GLUE, 'avail.is_null() || alloc.is_null() || !avail.is_aligned() || !alloc.is_aligned()', 'avail.is_null() || alloc.is_null()')]),
 ('glue-null-entry-accepted', [(GLUE, 'if e.is_null() { None } else { Some(EntryPtr(e)) }', 'Some(EntryPtr(e))')]),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def body(dis, f):
    b = dis[dis.index('<%s>:\n' % f):]; return b[:b.index('\n\n')] if '\n\n' in b else b
def frames(obj, names):
    dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', str(obj)], capture_output=True, text=True, check=True).stdout; out = {}
    for f in names:
        if '<%s>:\n' % f not in dis: out[f] = None; continue
        sub = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', body(dis, f)); out[f] = len(re.findall(r'\tpush ', body(dis, f))) * 8 + (int(sub[1], 16) if sub else 0)
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
        need(sha(ROOT / REF) == man['sources_sha256']['pmm-r4m3.c'], 'frozen accepted source')
        need(all(sha(ROOT / f) == h for f, h in man['unchanged_sha256'].items()), 'headers and the accepted R4-M1/R4-M2 Rust unchanged')
        # scope: production pmm.c is the frozen pmm.c with pmm_init_in and its helpers replaced by the note, nothing else
        frozen = (ROOT / REF).read_text(); prod = (ROOT / PMM).read_text()
        i = frozen.index('static uint64_t count_bits('); j = frozen.index('/*\n * R4-M2: pmm_alloc_in')
        need(all(h + '(' in frozen[i:j] for h in HELPERS + ('pmm_init_in',)) and prod.count(NOTE) == 1, 'the removed block is pmm_init_in and its helpers')
        need(frozen[:i] + NOTE + frozen[j:] == prod, 'production pmm.c differs from the frozen reference only by the moved functions')
        report['scope'] = {'moved_functions': list(HELPERS) + ['pmm_init_in'], 'other_bytes_changed': 0, 'frozen_pmm_c_sha256': sha(ROOT / REF), 'production_pmm_c_sha256': sha(ROOT / PMM)}
        # memory model, statically
        core = (ROOT / CORE).read_text(); glue = (ROOT / GLUE).read_text(); ccode = re.sub(r'//.*', '', core); gcode = re.sub(r'//.*', '', glue)
        need('#![forbid(unsafe_code)]' in core and not re.search(r'\bunsafe\b|\*const|\*mut|from_raw_parts|\bstatic\b|\bCell\b|\bas\b', ccode.replace('#![forbid(unsafe_code)]', '')), 'the core has no unsafe code, raw pointer, static or cast')
        need(not re.search(r'&\s*\*|&\s*mut\s*\*|from_raw_parts|\bCell\b|\bas_ref\b|\bas_mut\b|\bderef\b|\bstatic\b', gcode), 'the adapter forms no reference, slice or Cell over C memory and keeps no state')
        blocks = list(re.finditer(r'unsafe \{', glue))
        need(all('// SAFETY:' in glue[max(0, b.start() - 420):b.start()] for b in blocks), 'every unsafe block is justified')
        report['unsafe'] = {'core': 0, 'adapter_blocks': len(blocks), 'adapter_entry_points': glue.count('pub unsafe extern "C" fn'), 'references_to_c_memory': 0}
        # constants and layouts: a generated translation unit asserts every one against the C headers
        rust = dict(re.findall(r'^(?:pub )?const (\w+): (?:u64|u32|i32|usize) = ([0-9a-fx_]+);', core + glue, re.M))
        need(set(OFFSETS) | set(CONSTANTS) <= set(rust), 'every bound constant is defined')
        check = out / 'layout-check.c'
        check.write_text('#include <stddef.h>\n#include "pmm.h"\n#include "multiboot2.h"\n' + ''.join('_Static_assert((%s) == %sULL, "%s");\n' % (c, rust[n].replace('_', ''), n) for n, c in {**OFFSETS, **CONSTANTS}.items()))
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        run(['gcc', *warn, '-c', check, '-o', out / 'layout-check.o'], 'layout-check.txt'); report['constants_bound'] = len(OFFSETS) + len(CONSTANTS)
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        def ref_obj(tag, flags):
            o = out / ('ref-%s.o' % tag); run(['gcc', *flags, *warn, *renames, '-c', REF, '-o', o], 'ref-%s-build.txt' % tag); return o
        def host_rust(tag, d, lints='-Dwarnings'):
            root = out / ('root-%s.rs' % tag); lib = out / ('librust-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / M1GLUE)) + '] mod pmm;\n#[path=' + json.dumps(str(ROOT / M2GLUE)) + '] mod pmm_transition;\n#[path=' + json.dumps(str(d / GLUE)) + '] mod pmm_init;\n'
                            'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', lints, '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib', '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib, root
        def differential(tag, flags, ref, rust_lib, n, timeout=1800):
            b = out / ('diff-%s' % tag)
            run(['gcc', *flags, *warn, HARNESS, ref, rust_lib, *link, '-o', b], 'diff-%s-build.txt' % tag)
            try: text = run([b, n], 'diff-%s.txt' % tag, check=False, timeout=timeout)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b
            m = re.search(r'PMM INIT DIFFERENTIAL cases=(\d+) checks=(\d+) mismatches=(\d+) digest=([0-9a-f]{16})', text)
            return text, (dict(cases=int(m[1]), checks=int(m[2]), mismatches=int(m[3]), digest=m[4]) if m else None), b
        checked, _ = host_rust('checked', ROOT)
        report['differential'] = {}; cov = {}
        for tag, flags, lib in (('release', ['-O2'], archive), ('sanitizer', san + ['-DHOST_RUST'], checked)):
            text, r, _ = differential(tag, flags, ref_obj(tag, flags), lib, SEEDED)
            need(r and r['mismatches'] == 0 and 'MISMATCH' not in text and 'runtime error' not in text, 'frozen C and Rust initialization identical (%s): %s' % (tag, text[-600:]))
            cov[tag] = re.search(r'^PMM INIT COVERAGE .*$', text, re.M)[0]; report['differential'][tag] = r
        need(report['differential']['release']['digest'] == report['differential']['sanitizer']['digest'] and cov['release'] == cov['sanitizer'], 'transcript independent of build mode')
        counts = {k: [int(x) for x in v.split(',')] for k, v in re.findall(r'(\w+)=([\d,]+)', cov['release'])}
        need([len(counts[k]) for k in ('rc', 'layouts', 'refusals')] == [3, 8, 5] and all(x > 0 for v in counts.values() for x in v), 'every result, layout, refusal class and named class reached')
        report['coverage'] = cov['release']
        # named mutation controls (checked host build: a fault, an overflow abort or a mismatch counts; a build failure does not)
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 60, 'at least 60 named mutation controls')
        mref = ref_obj('mut', ['-O1'])
        def mutant(m):
            name, edits = m
            d = out / 'mutants' / name
            for f in (CORE, GLUE): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            for rel, old, new in edits:
                src = (d / rel).read_text(); need(src.count(old) == 1, 'unique mutation ' + name); (d / rel).write_text(src.replace(old, new))
            lib, root = host_rust('mutant-' + name, d, '--cap-lints=allow')
            text, r, b = differential('mutant-' + name, ['-O1', '-DHOST_RUST'], mref, lib, MUTANT_SEEDED, timeout=900)
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
        # linked kernel: ownership, panic freedom, callees, frames
        k = kdir / 'vos.bin'
        syms = subprocess.run(['nm', k], capture_output=True, text=True, check=True).stdout
        pobj = subprocess.run(['nm', kdir / 'pmm.o'], capture_output=True, text=True, check=True).stdout
        need(re.search(r'^[0-9a-f]+ T pmm_init_in$', syms, re.M), 'kernel defines pmm_init_in')
        need(not re.search(r' [TtWw] (pmm_init_in|%s)$' % '|'.join(HELPERS), pobj, re.M) and re.search(r' U pmm_init_in$', pobj, re.M), 'pmm.o defines neither pmm_init_in nor its helpers, and pmm_init calls the Rust one')
        need(not re.search(r'core::panicking|_ZN4core9panicking|panic_fmt|panic_bounds_check', syms), 'no panic machinery in the kernel')
        kmap = (kdir / 'kernel.map').read_text()
        need(re.search(r'\.text\.pmm_init_in\s*\n?\s*0x[0-9a-f]+\s+0x[0-9a-f]+ .*libvos_kernel\.a', kmap), 'link map takes pmm_init_in from the Rust archive')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', k], capture_output=True, text=True, check=True).stdout
        # everything pmm_init_in can reach: its own core functions (kept out of line by the compiler) and boot_mmap_entry, no recursion
        callees = lambda f: sorted(set(re.findall(r'(?:call|jmp)\s+[0-9a-f]+ <([^>+]+)>', body(dis, f))) - {f})
        reach = {}; todo = ['pmm_init_in']
        while todo:
            f = todo.pop()
            if f not in reach: reach[f] = callees(f); todo += reach[f]
        own = sorted(f for f in reach if 'pmm_init' in f and 'init_core' in f)
        need(set(reach) - set(own) <= {'pmm_init_in', 'boot_mmap_entry'} and not reach.get('boot_mmap_entry'), 'pmm_init_in reaches only its own core and boot_mmap_entry (which may be inlined): %s' % sorted(reach))
        fk = frames(k, list(reach))
        def depth(f, seen=()):
            need(f not in seen, 'no recursion under pmm_init_in'); return fk[f] + max([8 + depth(g, seen + (f,)) for g in reach[f]] or [0])
        b = body(dis, 'pmm_init_in'); calls = sorted(reach)
        kflags = ['-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx',
                  '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection']
        run(['gcc', '-std=gnu11', *kflags, '-Iarch/x86_64', '-c', REF, '-o', out / 'ref-kernel.o'], 'ref-kernel-build.txt')
        fr = frames(out / 'ref-kernel.o', C_FRAMES + ('pmm_init_in',)); fc = frames(kdir / 'pmm.o', C_FRAMES)
        need(all(fr[f] is not None and fc[f] is not None for f in C_FRAMES) and fr['pmm_init_in'] is not None, 'every frame measured')
        report['rust'] = {'pmm_init_in': {'frame': fk['pmm_init_in'], 'whole_path': depth('pmm_init_in'), 'frozen_c_frame': fr['pmm_init_in'], 'instructions': len(re.findall(r'^\s*[0-9a-f]+:\t(?!int3)', b, re.M)),
                                          'reaches': {('core helper %d' % own.index(f) if f in own else f): {'frame': fk[f], 'calls': len(reach[f])} for f in reach if f != 'pmm_init_in'}}}
        report['c_frames'] = {f: {'frozen': fr[f], 'production': fc[f]} for f in C_FRAMES}
        report['c_frame_growth'] = {f: v for f, v in report['c_frames'].items() if v['production'] > v['frozen']}
        report['source_sha256'] = {n: sha(ROOT / n) for n in [CORE, GLUE, PMM, 'arch/x86_64/pmm.h', 'arch/x86_64/rust/kernel_probe.rs', HARNESS, REF, MANIFEST, 'scripts/test-pmm-init-rust.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:6000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

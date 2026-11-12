#!/usr/bin/env python3
"""R4-V1: Rust canonical, check_va, check_nonleaf (vmm.c) and user_range (usercopy.c) against the frozen accepted C
(tests/reference/vmm-r4v1.c, usercopy-r4v1.c). Scope: production vmm.c and usercopy.c must equal the frozen files byte for byte
once the four forwarding bodies and the include line are replaced by the accepted ones, so every caller is unchanged. The Rust
core has no unsafe code, pointer, reference or state; the adapter has no unsafe block and scalar arguments only; the constants
are bound to memlayout.h and vmm.h. A three-way differential (tests/host/vmm-check-differential.c: frozen C, production C
forwarder, Rust export) runs exhaustive boundary domains and seeded 64-bit cases, and compares the user-copy callers by result
and lookup sequence, in a release build linked with the actual kernel archive and in an ASan/UBSan build linked with an
overflow-checked host build of the same Rust; digests and coverage must match with zero mismatches. Named mutation controls must
each be detected. The linked kernel is checked for Rust ownership, the callers' calls, no panic machinery and entry frames.
Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = 'kernel/rust/mm/vmm_check.rs'; GLUE = 'arch/x86_64/rust/vmm_check.rs'; VMM = 'arch/x86_64/vmm.c'; UC = 'arch/x86_64/usercopy.c'
REFV = 'tests/reference/vmm-r4v1.c'; REFU = 'tests/reference/usercopy-r4v1.c'; MANIFEST = 'tests/reference/vmm-r4v1.json'; HARNESS = 'tests/host/vmm-check-differential.c'
RUST = ('vos_rust_vmm_canonical', 'vos_rust_vmm_check_va', 'vos_rust_vmm_check_nonleaf', 'vos_rust_user_range')
CALLS = {'vmm_map': RUST[1:3], 'vmm_unmap': RUST[1:3], 'vmm_lookup': RUST[:1], 'user_writable': RUST[3:], 'copy_from_user': RUST[3:]}
C_FRAMES = ('vmm_map', 'vmm_unmap', 'vmm_lookup', 'user_writable', 'copy_to_user', 'copy_from_user')
COPIES = ('user_writable', 'copy_to_user', 'copy_from_user')
CASES, MUTANT_CASES = 10000000, 300000
# The accepted bodies that the forwarders replace: (file, production text, frozen text).
SCOPE = [
 (VMM, '#include "rust_kernel.h" /* R4-V1: canonical, check_va and check_nonleaf are decided in Rust. */\n', ''),
 (VMM, '    return vos_rust_vmm_canonical(va);\n', '    return va < LOWER_HALF_END || va >= UPPER_HALF_START;\n'),
 (VMM, '    return vos_rust_vmm_check_nonleaf(e, level);   /* PS first (reserved in a PML4E), then NONLEAF_BITS */\n',
  '    if (e & PTE_PS)\n        return level == 3 ? VMM_ECORRUPT : VMM_ELARGE;  /* PS is reserved in a PML4E */\n    if (e & ~NONLEAF_BITS)\n        return VMM_ECORRUPT;\n    return VMM_OK;\n'),
 (VMM, '    return vos_rust_vmm_check_va(va);   /* non-canonical, upper half, alignment, range: in this order */\n',
  '    if (!canonical(va))\n        return VMM_ENONCANON;\n    if (va >= UPPER_HALF_START)\n        return VMM_EKERNEL;\n    if (va % PAGE_SIZE != 0)\n        return VMM_EALIGN;\n'
  '    if (va < NULL_GUARD_END || va >= LOWER_MAP_END)\n        return VMM_ERANGE;\n    return VMM_OK;\n'),
 (UC, '#include "rust_kernel.h" /* R4-V1: user_range is decided in Rust. */\n', ''),
 (UC, '    return vos_rust_user_range(uptr, len);\n', '    return uptr >= USER_IMAGE_START && uptr < LOWER_HALF_END && len <= LOWER_HALF_END - uptr;\n'),
]
# Equivalent and excluded: swapping the non-canonical and upper-half checks of check_va (no address is both), and `%` against a
# mask for the alignment test.
KERNEL_CHECK = '    if va >= UPPER_HALF_START {\n        return EKERNEL;\n    }\n'; ALIGN_CHECK = '    if va % PAGE_SIZE != 0 {\n        return EALIGN;\n    }\n'
RANGE_CHECK = '    if va < NULL_GUARD_END || va >= LOWER_MAP_END {\n        return ERANGE;\n    }\n'
PS_CHECK = '    if e & PTE_PS != 0 {\n        return if level == 3 { ECORRUPT } else { ELARGE };\n    }\n'; BITS_CHECK = '    if e & !NONLEAF_BITS != 0 {\n        return ECORRUPT;\n    }\n'
UR = 'uptr >= USER_IMAGE_START && uptr < LOWER_HALF_END && len <= LOWER_HALF_END.wrapping_sub(uptr)'
ZERO = '    if (len == 0)\n        return 0;\n    if (!user_range(uptr, len))\n        return -VOS_EFAULT;\n\n'
MUTATIONS = [  # (name, file, exact text, replacement): each must make the differential mismatch, fault or abort
 ('canonical-lower-bound', CORE, 'va < LOWER_HALF_END || va >= UPPER_HALF_START', 'va <= LOWER_HALF_END || va >= UPPER_HALF_START'),
 ('canonical-upper-bound', CORE, 'va < LOWER_HALF_END || va >= UPPER_HALF_START', 'va < LOWER_HALF_END || va > UPPER_HALF_START'),
 ('canonical-and', CORE, 'va < LOWER_HALF_END || va >= UPPER_HALF_START', 'va < LOWER_HALF_END && va >= UPPER_HALF_START'),
 ('canonical-lower-only', CORE, 'va < LOWER_HALF_END || va >= UPPER_HALF_START', 'va < LOWER_HALF_END'),
 ('va-noncanon-result', CORE, '        return ENONCANON;', '        return ERANGE;'),
 ('va-kernel-bound', CORE, '    if va >= UPPER_HALF_START {\n        return EKERNEL;', '    if va > UPPER_HALF_START {\n        return EKERNEL;'),
 ('va-kernel-skipped', CORE, KERNEL_CHECK, ''),
 ('va-align-before-kernel', CORE, KERNEL_CHECK + ALIGN_CHECK, ALIGN_CHECK + KERNEL_CHECK),
 ('va-align-modulus', CORE, 'va % PAGE_SIZE != 0', 'va % (PAGE_SIZE / 2) != 0'),
 ('va-align-skipped', CORE, ALIGN_CHECK, ''),
 ('va-range-before-align', CORE, ALIGN_CHECK + RANGE_CHECK, RANGE_CHECK + ALIGN_CHECK),
 ('va-null-guard-bound', CORE, 'if va < NULL_GUARD_END ||', 'if va <= NULL_GUARD_END ||'),
 ('va-map-end-bound', CORE, '|| va >= LOWER_MAP_END {', '|| va > LOWER_MAP_END {'),
 ('va-null-guard-skipped', CORE, 'if va < NULL_GUARD_END || va >= LOWER_MAP_END {', 'if va >= LOWER_MAP_END {'),
 ('va-map-end-skipped', CORE, 'if va < NULL_GUARD_END || va >= LOWER_MAP_END {', 'if va < NULL_GUARD_END {'),
 ('nonleaf-ps-level', CORE, 'if level == 3 { ECORRUPT } else { ELARGE }', 'if level == 2 { ECORRUPT } else { ELARGE }'),
 ('nonleaf-ps-level-range', CORE, 'if level == 3 { ECORRUPT } else { ELARGE }', 'if level >= 3 { ECORRUPT } else { ELARGE }'),
 ('nonleaf-ps-swapped', CORE, 'if level == 3 { ECORRUPT } else { ELARGE }', 'if level == 3 { ELARGE } else { ECORRUPT }'),
 ('nonleaf-ps-ignored', CORE, PS_CHECK, ''),
 ('nonleaf-bits-before-ps', CORE, PS_CHECK + BITS_CHECK, BITS_CHECK + PS_CHECK),
 ('nonleaf-allows-nx', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 1 << 63) != 0 {'),
 ('nonleaf-allows-dirty', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 1 << 6) != 0 {'),
 ('nonleaf-allows-pwt', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 1 << 3) != 0 {'),
 ('nonleaf-allows-pcd', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 1 << 4) != 0 {'),
 ('nonleaf-allows-global', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 1 << 8) != 0 {'),
 ('nonleaf-allows-software-bits', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 7 << 9) != 0 {'),
 ('nonleaf-allows-high-bits', CORE, 'if e & !NONLEAF_BITS != 0 {', 'if e & !(NONLEAF_BITS | 0x7ff << 52) != 0 {'),
 ('nonleaf-refuses-accessed', CORE, 'PTE_P | PTE_W | PTE_U | PTE_A | PTE_ADDR;', 'PTE_P | PTE_W | PTE_U | PTE_ADDR;'),
 ('nonleaf-refuses-user', CORE, 'PTE_P | PTE_W | PTE_U | PTE_A | PTE_ADDR;', 'PTE_P | PTE_W | PTE_A | PTE_ADDR;'),
 ('nonleaf-address-mask', CORE, 'pub const PTE_ADDR: u64 = 0x000F_FFFF_FFFF_F000;', 'pub const PTE_ADDR: u64 = 0x0000_FFFF_FFFF_F000;'),
 ('nonleaf-corrupt-result', CORE, BITS_CHECK, BITS_CHECK.replace('ECORRUPT', 'ELARGE')),
 ('range-start-bound', CORE, UR, UR.replace('uptr >= USER_IMAGE_START', 'uptr > USER_IMAGE_START')),
 ('range-start-skipped', CORE, UR, UR.replace('uptr >= USER_IMAGE_START && ', '')),
 ('range-end-bound', CORE, UR, UR.replace('uptr < LOWER_HALF_END', 'uptr <= LOWER_HALF_END')),
 ('range-end-skipped', CORE, UR, UR.replace('uptr < LOWER_HALF_END && ', '')),
 ('range-length-bound', CORE, UR, UR.replace('len <= LOWER', 'len < LOWER')),
 ('range-length-base', CORE, UR, UR.replace('LOWER_HALF_END.wrapping_sub', 'LOWER_MAP_END.wrapping_sub')),
 ('range-length-ignored', CORE, UR, UR.replace(' && len <= LOWER_HALF_END.wrapping_sub(uptr)', '')),
 ('range-overflowing-sum', CORE, UR, UR.replace('len <= LOWER_HALF_END.wrapping_sub(uptr)', 'uptr.wrapping_add(len) <= LOWER_HALF_END')),
 ('glue-canonical-negated', GLUE, 'v::canonical(va) as i32', '!v::canonical(va) as i32'),
 ('glue-check-va-is-canonical', GLUE, '    v::check_va(va)', '    v::canonical(va) as i32'),
 ('glue-level-truncated', GLUE, 'v::check_nonleaf(e, level)', 'v::check_nonleaf(e, level as i8 as i32)'),
 ('glue-entry-truncated', GLUE, 'v::check_nonleaf(e, level)', 'v::check_nonleaf(e as u32 as u64, level)'),
 ('glue-length-truncated', GLUE, 'v::user_range(uptr, len as u64)', 'v::user_range(uptr, len as u32 as u64)'),
 ('glue-range-arguments-swapped', GLUE, 'v::user_range(uptr, len as u64)', 'v::user_range(len as u64, uptr)'),
 ('c-check-va-forwards-canonical', VMM, 'return vos_rust_vmm_check_va(va);', 'return !vos_rust_vmm_canonical(va);'),
 ('c-canonical-forwards-check-va', VMM, 'return vos_rust_vmm_canonical(va);', 'return vos_rust_vmm_check_va(va) == VMM_OK;'),
 ('c-nonleaf-level-shifted', VMM, 'return vos_rust_vmm_check_nonleaf(e, level);', 'return vos_rust_vmm_check_nonleaf(e, level - 1);'),
 ('c-user-range-inverted', UC, 'return vos_rust_user_range(uptr, len);', 'return !vos_rust_user_range(uptr, len);'),
 ('c-user-range-length-halved', UC, 'return vos_rust_user_range(uptr, len);', 'return vos_rust_user_range(uptr, len >> 1);'),
 ('c-writable-zero-length-shortcut-removed', UC, ZERO + '    /* Every destination', ZERO[ZERO.index('    if (!user_range'):] + '    /* Every destination'),
 ('c-copy-from-zero-length-shortcut-removed', UC, ZERO + '    uint64_t end = uptr + len;          /*', ZERO[ZERO.index('    if (!user_range'):] + '    uint64_t end = uptr + len;          /*'),
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
def cnum(text, name):   # a #define's integer value
    v = re.search(r'#define\s+%s\s+(.+?)\s*(?:/\*|$)' % name, text, re.M)[1]; return eval(re.sub(r'(?<=[0-9a-fA-F])(?:ULL|ull|u)\b', '', v), {'__builtins__': {}})
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
        need(sha(ROOT / REFV) == man['sources_sha256']['vmm-r4v1.c'] and sha(ROOT / REFU) == man['sources_sha256']['usercopy-r4v1.c'], 'frozen accepted sources')
        need(all(sha(ROOT / f) == h for f, h in man['unchanged_sha256'].items()), 'headers and the accepted R4-M1/R4-M2 Rust unchanged')
        # scope: undoing the four forwarders and the include line gives the frozen files exactly, so every caller is unchanged
        for prod, ref in ((VMM, REFV), (UC, REFU)):
            text = (ROOT / prod).read_text()
            for f, new, old in SCOPE:
                if f == prod: need(text.count(new) == 1, 'forwarder present once: ' + new.strip()[:50]); text = text.replace(new, old)
            need(text == (ROOT / ref).read_text(), prod + ' differs from the frozen reference only in the forwarded decisions')
        report['scope'] = {'forwarders': 4, 'include_lines': 2, 'other_bytes_changed': 0}
        # memory model, statically: scalars only
        core = re.sub(r'//.*', '', (ROOT / CORE).read_text()); glue = re.sub(r'//.*', '', (ROOT / GLUE).read_text())
        REFS = r'\*const|\*mut|&mut|[:(=,>]\s*&[^&]|\bstatic\b|\bmut\b|\[|\bimpl\b|\bstruct\b'    # pointers, borrows, state, slices, types
        need('#![forbid(unsafe_code)]' in core and not re.search(r'\bunsafe\b|\bas\b|' + REFS, core.replace('#![forbid(unsafe_code)]', '')), 'the core has no unsafe code, pointer, reference, slice, state or cast')
        need(not re.search(r'unsafe\s*\{|unsafe\s+(fn|extern|impl)|' + REFS, re.sub(r'#\[[^\]]*\]', '', glue)), 'the adapter has no unsafe block, pointer, reference or state')
        sigs = re.findall(r'#\[unsafe\(no_mangle\)\]\s*pub extern "C" fn (\w+)\(([^)]*)\) -> (\w+)', glue)
        need(sorted(s[0] for s in sigs) == sorted(RUST) and glue.count('unsafe') == 4 and all(s[2] == 'i32' and set(re.findall(r': (\w+)', s[1])) <= {'u64', 'i32', 'usize'} for s in sigs), 'four scalar exports')
        report['unsafe'] = {'core': 0, 'adapter_blocks': 0, 'adapter_no_mangle_attributes': 4, 'pointer_parameters': 0}
        # the Rust constants are the header constants
        hdr = (ROOT / 'arch/x86_64/memlayout.h').read_text() + (ROOT / 'arch/x86_64/vmm.h').read_text(); rc = {}
        for name, expr in re.findall(r'pub const (\w+): (?:u64|i32) = ([^;]+);', core): rc[name] = eval(expr.replace('_', '') if re.fullmatch(r'[0-9a-fA-Fx_]+', expr) else expr, {'__builtins__': {}}, dict(rc))
        for name in ('PAGE_SIZE', 'LOWER_HALF_END', 'UPPER_HALF_START', 'NULL_GUARD_END', 'LOWER_MAP_END', 'USER_IMAGE_START', 'PTE_P', 'PTE_W', 'PTE_U', 'PTE_A', 'PTE_PS', 'PTE_ADDR'):
            need(rc[name] == cnum(hdr, name), 'constant ' + name)
        for name in ('OK', 'EALIGN', 'ENONCANON', 'ERANGE', 'EKERNEL', 'ELARGE', 'ECORRUPT'): need(rc[name] == cnum(hdr, 'VMM_' + name), 'constant VMM_' + name)
        need(re.search(r'#define NONLEAF_BITS\s+\(PTE_P \| PTE_W \| PTE_U \| PTE_A \| PTE_ADDR\)', (ROOT / REFV).read_text()) and rc['NONLEAF_BITS'] == rc['PTE_P'] | rc['PTE_W'] | rc['PTE_U'] | rc['PTE_A'] | rc['PTE_ADDR'], 'constant NONLEAF_BITS')
        report['constants_bound'] = len(rc)
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        def side(tag, flags, who, kind, src):   # one vmm.c or usercopy.c with its statics exported as <who>_*; nothing else stays global
            o = out / ('%s-%s-%s.o' % (who, kind, tag)); names = ['canonical', 'check_va', 'check_nonleaf'] if kind == 'vmm' else ['user_range', *COPIES]
            ren = [] if kind == 'vmm' else ['-D%s=%s_%s' % (f, who, f) for f in COPIES]
            run(['gcc', *flags, *warn, '-DSIDE=' + who, '-DSIDE_' + ('VMM' if kind == 'vmm' else 'USERCOPY'), '-DSRC="%s"' % src, *ren, '-c', HARNESS, '-o', o], '%s-%s-%s-build.txt' % (who, kind, tag))
            run(['objcopy', *[x for n in names for x in ('-G', '%s_%s' % (who, n))], o], '%s-%s-%s-objcopy.txt' % (who, kind, tag))
            return o
        def sides(tag, flags, who, d): return [side(tag, flags, who, 'vmm', d / (REFV if who == 'ref' else VMM)), side(tag, flags, who, 'uc', d / (REFU if who == 'ref' else UC))]
        def host_rust(tag, d, lints='-Dwarnings'):
            root = out / ('root-%s.rs' % tag); lib = out / ('librust-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(d / GLUE)) + '] mod vmm_check;\nunsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', lints, '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib', '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib, root
        def differential(tag, flags, objs, rust, n, timeout=1800):
            b = out / ('diff-%s' % tag)
            run(['gcc', *flags, *warn, HARNESS, *objs, rust, *link, '-o', b], 'diff-%s-build.txt' % tag)
            try: text = run([b, n], 'diff-%s.txt' % tag, check=False, timeout=timeout)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b
            m = re.search(r'VMM CHECK DIFFERENTIAL cases=(\d+) mismatches=(\d+) digest=([0-9a-f]{16})', text)
            return text, (dict(cases=int(m[1]), mismatches=int(m[2]), digest=m[3]) if m else None), b
        checked, _ = host_rust('checked', ROOT)
        report['differential'] = {}; cov = {}
        for tag, flags, rust in (('release', ['-O2'], archive), ('sanitizer', san, checked)):
            text, r, _ = differential(tag, flags, sides(tag, flags, 'ref', ROOT) + sides(tag, flags, 'prod', ROOT), rust, CASES)
            need(r and r['mismatches'] == 0 and 'MISMATCH' not in text and 'runtime error' not in text, 'frozen C, production C and Rust identical (%s): %s' % (tag, text[-600:]))
            cov[tag] = re.search(r'^VMM CHECK COVERAGE .*$', text, re.M)[0]; report['differential'][tag] = r
        need(report['differential']['release']['digest'] == report['differential']['sanitizer']['digest'] and cov['release'] == cov['sanitizer'], 'transcript independent of build mode')
        counts = {k: [int(x) for x in v.split(',')] for k, v in re.findall(r'(\w+)=([\d,]+)', cov['release'])}
        need([len(counts[k]) for k in ('canonical', 'check_va', 'check_nonleaf', 'user_range', 'callers')] == [3, 5, 4, 5, 4] and all(x > 0 for v in counts.values() for x in v), 'every result class of every decision and caller reached')
        report['coverage'] = cov['release']
        # named mutation controls (checked host build: overflow and panic aborts count as detection)
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 40, 'at least 40 named mutation controls')
        mref = sides('mut', ['-O1'], 'ref', ROOT); mprod = sides('mut', ['-O1'], 'prod', ROOT); mlib, _ = host_rust('mut', ROOT, '--cap-lints=allow')
        def mutant(m):
            name, rel, old, new = m
            d = out / 'mutants' / name
            for f in (CORE, GLUE, VMM, UC): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            src = (d / rel).read_text(); need(src.count(old) == 1, 'unique mutation ' + name); (d / rel).write_text(src.replace(old, new))
            made = []
            if rel in (VMM, UC): prod = sides('mutant-' + name, ['-O1'], 'prod', d); lib = mlib; made += prod
            else: prod = mprod; lib, root = host_rust('mutant-' + name, d, '--cap-lints=allow'); made += [lib, root]
            text, r, b = differential('mutant-' + name, ['-O1'], mref + prod, lib, MUTANT_CASES, timeout=600)
            shutil.rmtree(d)
            for f in made + [b]: f.unlink(missing_ok=True)
            return name, r, text
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, len(os.sched_getaffinity(0))))) as pool:
            results = list(pool.map(mutant, MUTATIONS))
        missed = [n for n, r, _ in results if r is not None and r['mismatches'] == 0]
        need(not missed, 'mutations not detected: %s' % missed)
        for name, r, text in results:
            ex = next((x['exit'] for x in report['commands'] if x['log'] == 'diff-mutant-%s.txt' % name), None)
            report['mutations'][name] = {'detected': True, 'mismatches': r['mismatches'] if r else ('hang' if text == 'TIMEOUT' else 'abnormal exit (fault or abort)'), 'exit': ex}
        # linked kernel: ownership, the callers' calls, panic freedom, frames
        k = kdir / 'vos.bin'
        syms = subprocess.run(['nm', k], capture_output=True, text=True, check=True).stdout; kmap = (kdir / 'kernel.map').read_text()
        need(all(re.search(r'^[0-9a-f]+ T %s$' % f, syms, re.M) for f in RUST), 'kernel defines every decision')
        need(all(re.search(r'\.text\.%s\s*\n?\s*0x[0-9a-f]+\s+0x[0-9a-f]+ .*libvos_kernel\.a' % f, kmap) for f in RUST), 'link map takes every decision from the Rust archive')
        for o, fs in (('vmm.o', RUST[:3]), ('usercopy.o', RUST[3:])):
            osym = subprocess.run(['nm', kdir / o], capture_output=True, text=True, check=True).stdout
            need(all(re.search(r' U %s$' % f, osym, re.M) for f in fs) and not any(re.search(r' [TtWw] %s$' % f, osym, re.M) for f in RUST), o + ' calls the Rust decisions and defines none')
        need(not re.search(r'core::panicking|_ZN4core9panicking|panic_fmt|panic_bounds_check', syms), 'no panic machinery in the kernel')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', k], capture_output=True, text=True, check=True).stdout
        for f, wanted in CALLS.items():
            got = set(re.findall(r'call\s+[0-9a-f]+ <([^>]+)>', body(dis, f))); need(set(wanted) <= got, '%s calls %s' % (f, sorted(set(wanted) - got)))
        report['rust'] = {}
        for f in RUST:
            b = body(dis, f); need(not re.search(r'\bcall\b', b) and 'rsp' not in b, 'leaf decision without calls or stack use: ' + f)
            report['rust'][f] = {'frame': 0, 'instructions': len(re.findall(r'^\s*[0-9a-f]+:\t(?!int3)', b, re.M))}
        kflags = ['-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx',
                  '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection']
        fr = {}
        for ref, tag, names in ((REFV, 'vmm', C_FRAMES[:3]), (REFU, 'usercopy', C_FRAMES[3:])):
            run(['gcc', '-std=gnu11', *kflags, '-Iarch/x86_64', '-c', ref, '-o', out / ('ref-kernel-%s.o' % tag)], 'ref-kernel-%s-build.txt' % tag); fr.update(frames(out / ('ref-kernel-%s.o' % tag), names))
        fc = dict(frames(kdir / 'vmm.o', C_FRAMES[:3]), **frames(kdir / 'usercopy.o', C_FRAMES[3:]))
        need(all(fr[f] is not None and fc[f] is not None for f in C_FRAMES), 'every caller frame measured on both sides')
        report['c_frames'] = {f: {'frozen': fr[f], 'production': fc[f]} for f in C_FRAMES}
        report['c_frame_growth'] = {f: v for f, v in report['c_frames'].items() if v['production'] > v['frozen']}
        report['source_sha256'] = {n: sha(ROOT / n) for n in [CORE, GLUE, VMM, UC, 'arch/x86_64/rust_kernel.h', 'arch/x86_64/rust/kernel_probe.rs', HARNESS, REFV, REFU, MANIFEST, 'scripts/test-vmm-check.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:6000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

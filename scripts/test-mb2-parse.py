#!/usr/bin/env python3
"""A2 hosted verification of the Rust Multiboot2 parser (mb2_parse).

mb2_parse is Rust (kernel/rust/mb2/parse.rs, arch/x86_64/rust/mb2_parse.rs) and comes from the kernel archive; the accepted C
parser is kept verbatim in tests/host/reference/bootinfo_ref.c as the frozen differential reference (hash-checked here).
Gates, in order (the first failure stops the run and keeps its logs):
  1. the reference is the accepted file (SHA-256), production bootinfo.c no longer defines mb2_parse, the walk has no unsafe;
  2. production ownership: the archive and vos.bin define mb2_parse from the Rust archive, bootinfo.o does not, the call sites
     resolve to it, and vos.bin has no core::panicking / core::fmt symbol;
  3. stack: the Rust frame is at most 512 bytes and at most 256 bytes above the C reference frame;
  4. the differential (tests/host/mb2-differential.c) against the production archive and against the same sources built for
     the host with overflow checks and abort-on-panic;
  5. negative controls: each broken reference (one decision each) must be detected by the differential.

Example: make ARCH=x86_64 BUILD=build/x86_64 RUSTC=... iso, then
  scripts/test-mb2-parse.py --archive build/x86_64/rust/libvos_kernel.a --kernel-dir build/x86_64 --rustc ... --out build/mb2-ci
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / 'tests/host/reference/bootinfo_ref.c'
# SHA-256 of arch/x86_64/bootinfo.c at the accepted baseline c77dabb2d84228120109c1e0f547264d99e6485e.
REF_SHA256 = '8a10854a27f94f5b4de9fdb2bdc9306beffa5149f556d48461e5fea7609251da'
RENAMES = ['-Dmb2_parse=mb2_parse_reference', '-Dboot_framebuffer=boot_framebuffer_reference',
           '-Dboot_mmap_entry=boot_mmap_entry_reference', '-Dboot_reserve=boot_reserve_reference']
MUTANTS = [
    ('tag-size-bound', 'tag->size > total - off', 'tag->size >= total - off'),
    ('tag-size-minimum', 'tag->size < sizeof(*tag) ||', 'tag->size <= sizeof(*tag) ||'),
    ('module-range', 'm->mod_start >= m->mod_end', 'm->mod_start > m->mod_end'),
    ('module-limit', 'm->mod_end > BOOT_MAP_LIMIT', 'm->mod_end >= BOOT_MAP_LIMIT'),
    ('module-count', 'bi->module_count >= BOOT_MAX_MODULES', 'bi->module_count > BOOT_MAX_MODULES'),
    ('mmap-entry-alignment', 'm->entry_size % 8 != 0', 'm->entry_size % 4 != 0'),
    ('mmap-version', 'm->entry_version != 0', 'm->entry_version > 1'),
    ('mmap-partial-entry', '(tag->size - sizeof(*m)) % m->entry_size != 0', '(tag->size - sizeof(*m)) % m->entry_size > 4'),
    ('end-tag-size', 'if (tag->size != sizeof(*tag))', 'if (0)'),
    ('tag-alignment', '(tag->size + MB2_TAG_ALIGN - 1) & ~(uint32_t)(MB2_TAG_ALIGN - 1)', 'tag->size'),
    ('framebuffer-first', 'if (!bi->fb_tags++)', 'if ((bi->fb_tags++, 1))'),
    ('efi-mmap-first', 'if (!bi->efi_mmap_tags++)', 'if ((bi->efi_mmap_tags++, 1))'),
    ('total-maximum', 'total > MB2_INFO_MAX', 'total >= MB2_INFO_MAX'),
    ('address-alignment', 'info_phys % MB2_TAG_ALIGN != 0 ||', ''),
    ('address-limit', 'info_phys > BOOT_MAP_LIMIT - sizeof(struct mb2_info)', 'info_phys >= BOOT_MAP_LIMIT - sizeof(struct mb2_info)'),
    ('duplicate-command-line', 'if (bi->cmdline)', 'if (0)'),
    ('no-memory-map-allowed', 'return bi->mmap ? NULL : "no memory map";', 'return NULL;'),
    ('empty-string-terminated', "return len > 0 && s[len - 1] == '\\0';", "return s[len - 1] == '\\0';"),
    ('message-text', '"no end tag"', '"no end tagg"'),
    ('output-not-zeroed', 'memset(bi, 0, sizeof(*bi));', ''),
]


def frame_bytes(disasm):
    """Pushes plus the `sub $N,%rsp` of one function (the return address is not counted, as in the A1 measurement)."""
    n = 0
    for line in disasm.splitlines():
        if re.search(r'\bpush\b', line):
            n += 8
        m = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', line)
        if m:
            n += int(m.group(1), 16)
    return n


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--kernel-dir', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--rustc', default=os.environ.get('RUSTC'))
    a = p.parse_args()
    if not a.rustc:
        p.error('--rustc or RUSTC is required (the hosted overflow-checked build of the parser)')
    archive, kdir, out = a.archive.resolve(), a.kernel_dir.resolve(), a.out.resolve()
    if out.exists():
        p.error('output already exists: preserve it and choose a fresh path')
    out.mkdir(parents=True)
    result = {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest()}

    def run(args, name, expect=0, timeout=300, env=None):
        with (out / name).open('x') as stream:
            r = subprocess.run(list(map(str, args)), cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout,
                               env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(env or {})))
        if r.returncode != expect:
            raise RuntimeError(f'{name}: exit {r.returncode}, expected {expect}')
        return (out / name).read_text()

    # 1. reference identity and static facts
    ref_src = REF.read_text()
    if hashlib.sha256(REF.read_bytes()).hexdigest() != REF_SHA256:
        raise RuntimeError('tests/host/reference/bootinfo_ref.c is not the accepted arch/x86_64/bootinfo.c')
    prod = (ROOT / 'arch/x86_64/bootinfo.c').read_text()
    glue = (ROOT / 'arch/x86_64/rust/mb2_parse.rs').read_text()
    walk = re.sub(r'//.*$', '', (ROOT / 'kernel/rust/mb2/parse.rs').read_text(), flags=re.M)
    if re.search(r'^const char \*mb2_parse\(', prod, re.M) or 'static int terminated' in prod:
        raise RuntimeError('production bootinfo.c still defines mb2_parse')
    if not re.search(r'^const char \*mb2_parse\(', ref_src, re.M):
        raise RuntimeError('the reference lost mb2_parse')
    if glue.count('pub unsafe extern "C" fn mb2_parse') != 1 or re.search(r'\bunsafe\b|\bextern\b|\bstd::|\balloc\b', walk):
        raise RuntimeError('the walk must be safe, pure Rust and the glue must export mb2_parse exactly once')
    result['reference_sha256'] = REF_SHA256
    # 2. production ownership
    if ' T mb2_parse\n' not in subprocess.check_output(['nm', str(archive)], text=True):
        raise RuntimeError('the archive does not define mb2_parse')
    if ' T mb2_parse\n' in subprocess.check_output(['nm', str(kdir / 'bootinfo.o')], text=True):
        raise RuntimeError('bootinfo.o still defines mb2_parse')
    vos = kdir / 'vos.bin'
    nm = subprocess.check_output(['nm', str(vos)], text=True)
    m = re.search(r'^([0-9a-f]+) T mb2_parse$', nm, re.M)
    if not m:
        raise RuntimeError('vos.bin does not define mb2_parse')
    kmap = (kdir / 'kernel.map').read_text()
    if not re.search(r'\.text\.mb2_parse\s*\n\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+\S*libvos_kernel\.a', kmap):
        raise RuntimeError('the link map does not take mb2_parse from the Rust archive')
    dis = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(vos)], text=True)
    calls = len(re.findall(r'call\s+' + m.group(1) + r' <mb2_parse>', dis))
    if calls < 1:
        raise RuntimeError('no call site resolves to the Rust mb2_parse')
    bad = [l for l in subprocess.check_output(['nm', '-C', str(vos)], text=True).splitlines() if re.search(r'core::(panicking|fmt)', l)]
    if bad:
        raise RuntimeError('panic/format symbols in vos.bin: ' + bad[0])
    result['ownership'] = {'mb2_parse_address': m.group(1), 'call_sites': calls, 'panic_fmt_symbols': 0,
                           'defined_by': 'libvos_kernel.a', 'bootinfo_o_defines_it': False}
    cc = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie', '-Iarch/x86_64']
    san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize=alignment', '-fno-omit-frame-pointer']
    tmp = Path(tempfile.mkdtemp(prefix='mb2-a2-'))
    try:
        run(cc + san + RENAMES + ['-c', REF, '-o', out / 'reference.o'], 'reference-build.txt')
        # 3. stack: the frames along the call chain (the wrapper and the walk it calls, when the compiler kept it out of line)
        def frame_of(binary, symbol):
            return frame_bytes(subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', f'--disassemble={symbol}', str(binary)], text=True))
        walk = re.search(r'^[0-9a-f]+ [tT] (\S*mb2_parse\S*5parse4walk\S*)$', nm, re.M)
        rust_frame = frame_of(vos, 'mb2_parse') + (frame_of(vos, walk.group(1)) if walk else 0)
        # the C reference built with the kernel's own flags, so its frame is the one the kernel had
        kflags = ['-std=gnu11', '-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin',
                  '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx', '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone',
                  '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection', '-Iarch/x86_64']
        run(['gcc'] + kflags + RENAMES + ['-c', REF, '-o', tmp / 'reference-kernel.o'], 'reference-kernel-build.txt')
        c_frame = frame_of(tmp / 'reference-kernel.o', 'mb2_parse_reference')
        if rust_frame > 512 or rust_frame > c_frame + 256:
            raise RuntimeError(f'stack {rust_frame} B (C reference {c_frame} B) exceeds the budget')
        result['stack'] = {'rust_chain_bytes': rust_frame, 'walk_kept_out_of_line': bool(walk), 'c_reference_bytes': c_frame,
                           'ceiling': 512, 'increase_ceiling': 256}
        # 4. differential
        env = {'ASAN_OPTIONS': 'detect_leaks=0'}
        run(cc + san + ['-Wl,--gc-sections', 'tests/host/mb2-differential.c', out / 'reference.o', archive, '-o', out / 'differential'],
            'differential-build.txt')
        d = run([out / 'differential'], 'differential.txt', env=env, timeout=900).strip().splitlines()
        result['differential_archive_build'] = d[0]
        result['differential_outcomes'] = d[1:]
        run([a.rustc, '--edition=2024', '-Dwarnings', '--crate-type=staticlib', '--crate-name', 'mb2_host', '-Cpanic=abort',
             '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '-Copt-level=1', 'tests/rust/mb2_parse_host.rs', '-o', tmp / 'libmb2_host.a'],
            'host-rust-build.txt')
        run(cc + san + ['-Wl,--gc-sections', 'tests/host/mb2-differential.c', out / 'reference.o', tmp / 'libmb2_host.a', '-o', tmp / 'differential-host'],
            'differential-host-build.txt')
        h = run([tmp / 'differential-host'], 'differential-host.txt', env=env, timeout=900).strip().splitlines()
        result['differential_overflow_checked_host_build'] = h[0]
        # 5. negative controls: a broken reference must be detected (a mismatch, or a guard-page fault)
        controls = {}
        for name, old, new in MUTANTS:
            if ref_src.count(old) != 1 or old == new:
                raise RuntimeError(f'negative control {name}: anchor occurs {ref_src.count(old)} times')
            (tmp / 'mutant.c').write_text(ref_src.replace(old, new))
            subprocess.run(cc + ['-O1'] + RENAMES + ['-c', tmp / 'mutant.c', '-o', tmp / 'mutant.o'], cwd=ROOT, check=True, capture_output=True)
            subprocess.run(cc + ['-O1', '-Wl,--gc-sections', 'tests/host/mb2-differential.c', tmp / 'mutant.o', archive, '-o', tmp / 'mutant-differential'],
                           cwd=ROOT, check=True, capture_output=True)
            r = subprocess.run([tmp / 'mutant-differential'], cwd=tmp, capture_output=True, text=True, timeout=300)
            controls[name] = ('mismatch' if r.returncode == 1 and 'MISMATCH' in r.stderr else
                              'guard-page fault' if r.returncode < 0 else 'NOT DETECTED')
        if any(v == 'NOT DETECTED' for v in controls.values()):
            raise RuntimeError(f'the differential missed a broken reference: {controls}')
        result['differential_detects_broken_references'] = controls
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    result['pass'] = True
    (out / 'results.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()

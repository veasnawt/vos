#!/usr/bin/env python3
"""A3 hosted verification of the Rust framebuffer validation (boot_framebuffer).

boot_framebuffer is Rust (kernel/rust/mb2/framebuffer.rs, arch/x86_64/rust/fb_validate.rs) and comes from the kernel archive; the
accepted C is kept verbatim in tests/host/reference/bootinfo_ref.c (the A2 reference, hash-checked here) as the frozen reference.
Gates, in order (the first failure stops the run and keeps its logs):
  1. the reference is the accepted file (SHA-256), production bootinfo.c no longer defines boot_framebuffer, the validation has no unsafe;
  2. production ownership: the archive and vos.bin define boot_framebuffer from the Rust archive, bootinfo.o does not, the call site
     resolves to it, and vos.bin has no core::panicking / core::fmt symbol;
  3. stack: the Rust frames along the call chain are at most 512 bytes and at most 256 bytes above the C reference frame;
  4. the differential (tests/host/fb-differential.c) against the production archive and against the same sources built for the
     host with overflow checks and abort-on-panic;
  5. negative controls: each broken reference (one decision each) must be detected by the differential.

Example: make ARCH=x86_64 BUILD=build/x86_64 RUSTC=... iso, then
  scripts/test-fb-validate.py --archive build/x86_64/rust/libvos_kernel.a --kernel-dir build/x86_64 --rustc ... --out build/a3-fb-ci
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
    ('width-minimum', 't->width < BOOT_FB_MIN_W ||', 't->width <= BOOT_FB_MIN_W ||'),
    ('height-minimum', 't->height < BOOT_FB_MIN_H ||', 't->height <= BOOT_FB_MIN_H ||'),
    ('height-maximum', 't->height > BOOT_FB_MAX_DIM)', 't->height >= BOOT_FB_MAX_DIM)'),
    ('pitch-alignment', 't->pitch % 4 ||', 't->pitch % 8 ||'),
    ('pitch-below-width', '(uint64_t)t->pitch < (uint64_t)t->width * 4', '(uint64_t)t->pitch <= (uint64_t)t->width * 4'),
    ('pitch-maximum', 't->pitch > BOOT_FB_MAX_PITCH', 't->pitch >= BOOT_FB_MAX_PITCH'),
    ('size-maximum', 'size > BOOT_FB_MAX_BYTES', 'size >= BOOT_FB_MAX_BYTES'),
    ('address-alignment', 't->addr % 4 ||', 't->addr % 8 ||'),
    ('address-zero', 't->addr == 0 ||', ''),
    ('address-wrap', 't->addr + size < t->addr', '0'),
    ('bits-per-pixel', 't->bpp != 32', 't->bpp < 32'),
    ('channel-red-position', 'c->red_pos != 16', 'c->red_pos != 0'),
    ('channel-layout-length', 't->tag.size < sizeof(*t) + sizeof(struct mb2_fb_rgb)', 't->tag.size <= sizeof(*t) + sizeof(struct mb2_fb_rgb)'),
    ('tag-truncated', 't->tag.size < sizeof(*t))', 't->tag.size <= sizeof(*t))'),
    ('duplicate-count', 'bi->fb_tags > 1', 'bi->fb_tags > 2'),
    ('ega-address', 't->addr != 0xB8000', 't->addr != 0xB8001'),
    ('ega-bytes', 'fb->size = 4000;', 'fb->size = 4096;'),
    ('ega-bits', 't->bpp != 16', 't->bpp != 8'),
    ('output-not-zeroed', 'memset(fb, 0, sizeof(*fb));', ''),
    ('message-text', '"framebuffer larger than 32 MiB"', '"framebuffer larger than 32 MB"'),
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
    glue = (ROOT / 'arch/x86_64/rust/fb_validate.rs').read_text()
    walk = re.sub(r'//.*$', '', (ROOT / 'kernel/rust/mb2/framebuffer.rs').read_text(), flags=re.M)
    if re.search(r'^const char \*boot_framebuffer\(', prod, re.M):
        raise RuntimeError('production bootinfo.c still defines boot_framebuffer')
    if not re.search(r'^const char \*boot_framebuffer\(', ref_src, re.M):
        raise RuntimeError('the reference lost boot_framebuffer')
    if glue.count('pub unsafe extern "C" fn boot_framebuffer') != 1 or re.search(r'\bunsafe\b|\bextern\b|\bstd::|\balloc\b', walk):
        raise RuntimeError('the validation must be safe, pure Rust and the glue must export boot_framebuffer exactly once')
    result['reference_sha256'] = REF_SHA256
    # 2. production ownership
    if ' T boot_framebuffer\n' not in subprocess.check_output(['nm', str(archive)], text=True):
        raise RuntimeError('the archive does not define boot_framebuffer')
    if ' T boot_framebuffer\n' in subprocess.check_output(['nm', str(kdir / 'bootinfo.o')], text=True):
        raise RuntimeError('bootinfo.o still defines boot_framebuffer')
    vos = kdir / 'vos.bin'
    nm = subprocess.check_output(['nm', str(vos)], text=True)
    m = re.search(r'^([0-9a-f]+) T boot_framebuffer$', nm, re.M)
    if not m:
        raise RuntimeError('vos.bin does not define boot_framebuffer')
    kmap = (kdir / 'kernel.map').read_text()
    if not re.search(r'\.text\.boot_framebuffer\s*\n\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+\S*libvos_kernel\.a', kmap):
        raise RuntimeError('the link map does not take boot_framebuffer from the Rust archive')
    dis = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(vos)], text=True)
    calls = len(re.findall(r'call\s+' + m.group(1) + r' <boot_framebuffer>', dis))
    if calls < 1:
        raise RuntimeError('no call site resolves to the Rust boot_framebuffer')
    bad = [l for l in subprocess.check_output(['nm', '-C', str(vos)], text=True).splitlines() if re.search(r'core::(panicking|fmt)', l)]
    if bad:
        raise RuntimeError('panic/format symbols in vos.bin: ' + bad[0])
    result['ownership'] = {'boot_framebuffer_address': m.group(1), 'call_sites': calls, 'panic_fmt_symbols': 0,
                           'defined_by': 'libvos_kernel.a', 'bootinfo_o_defines_it': False}
    cc = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie', '-Iarch/x86_64']
    san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize=alignment', '-fno-omit-frame-pointer']
    tmp = Path(tempfile.mkdtemp(prefix='fb-a3-'))
    try:
        run(cc + san + RENAMES + ['-c', REF, '-o', out / 'reference.o'], 'reference-build.txt')
        # 3. stack: the frames along the call chain (the wrapper and the walk it calls, when the compiler kept it out of line)
        def frame_of(binary, symbol):
            return frame_bytes(subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', f'--disassemble={symbol}', str(binary)], text=True))
        callees = re.findall(r'^[0-9a-f]+ t (\S*fb_validate\S*11framebuffer\S*)$', nm, re.M)
        rust_frame = frame_of(vos, 'boot_framebuffer') + sum(frame_of(vos, c) for c in callees)
        # the C reference built with the kernel's own flags, so its frame is the one the kernel had
        kflags = ['-std=gnu11', '-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin',
                  '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx', '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone',
                  '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection', '-Iarch/x86_64']
        run(['gcc'] + kflags + RENAMES + ['-c', REF, '-o', tmp / 'reference-kernel.o'], 'reference-kernel-build.txt')
        c_frame = frame_of(tmp / 'reference-kernel.o', 'boot_framebuffer_reference')
        if rust_frame > 512 or rust_frame > c_frame + 256:
            raise RuntimeError(f'stack {rust_frame} B (C reference {c_frame} B) exceeds the budget')
        result['stack'] = {'rust_chain_bytes': rust_frame, 'out_of_line_callees': len(callees), 'c_reference_bytes': c_frame,
                           'ceiling': 512, 'increase_ceiling': 256}
        # 4. differential
        env = {'ASAN_OPTIONS': 'detect_leaks=0'}
        run(cc + san + ['-Wl,--gc-sections', 'tests/host/fb-differential.c', out / 'reference.o', archive, '-o', out / 'differential'],
            'differential-build.txt')
        d = run([out / 'differential'], 'differential.txt', env=env, timeout=900).strip().splitlines()
        result['differential_archive_build'] = d[0]
        result['differential_outcomes'] = d[1:]
        run([a.rustc, '--edition=2024', '-Dwarnings', '--crate-type=staticlib', '--crate-name', 'fb_host', '-Cpanic=abort',
             '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '-Copt-level=1', 'tests/rust/fb_validate_host.rs', '-o', tmp / 'libfb_host.a'],
            'host-rust-build.txt')
        run(cc + san + ['-Wl,--gc-sections', 'tests/host/fb-differential.c', out / 'reference.o', tmp / 'libfb_host.a', '-o', tmp / 'differential-host'],
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
            subprocess.run(cc + ['-O1', '-Wl,--gc-sections', 'tests/host/fb-differential.c', tmp / 'mutant.o', archive, '-o', tmp / 'mutant-differential'],
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

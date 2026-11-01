#!/usr/bin/env python3
"""B1 hosted verification of the Rust boot reservation boundary: boot_reserve, boot_mmap_entry, the reservation table
(resv_init, resv_add, resv_overlaps) and the C half of the EFI exclusion (efi_memory_exclude, efi_memory_firmware_frame,
efi_memory_strerror, efi_memory_what).

The nine symbols are Rust (kernel/rust/boot/reserve.rs, arch/x86_64/rust/boot_resv.rs) and come from the kernel archive. The three
C files they replaced (arch/x86_64/{bootinfo,resv,efi_memory}.c) are retired from the production build and the tree; the accepted
C is kept verbatim in tests/host/reference/{bootinfo,resv,efi_memory}_ref.c as the frozen references (hash-checked here).
Gates, in order (the first failure stops the run and keeps its logs):
  1. the references are the accepted files (SHA-256), the three production files are gone and not in arch.mk, the decisions have
     no unsafe, each symbol is exported exactly once;
  2. production ownership: the archive and vos.bin define every symbol from the Rust archive, none of the three C objects exists,
     the call sites resolve to the Rust code, and vos.bin has no core::panicking / core::fmt symbol;
  3. stack: the Rust frames along each call chain are at most 512 bytes and at most 256 bytes above the C reference frame;
  4. the differential (tests/host/boot-resv-differential.c) against the production archive and against the same sources built for
     the host with overflow checks and abort-on-panic;
  5. negative controls: each broken reference (one decision each) must be detected by the differential.

Example: make ARCH=x86_64 BUILD=build/x86_64 RUSTC=... iso, then
  scripts/test-boot-resv.py --archive build/x86_64/rust/libvos_kernel.a --kernel-dir build/x86_64 --rustc ... --out build/b1-resv-ci
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
REFS = {
    'bootinfo_ref': ('tests/host/reference/bootinfo_ref.c', '8a10854a27f94f5b4de9fdb2bdc9306beffa5149f556d48461e5fea7609251da'),  # arch/x86_64/bootinfo.c at c77dabb2
    'resv_ref': ('tests/host/reference/resv_ref.c', '3f28530b5a04bcd206eefc14ee4805ebdee8be638633ecd7213915204408c656'),  # arch/x86_64/resv.c at 1b1cb5d7
    'efi_memory_ref': ('tests/host/reference/efi_memory_ref.c', '31aee19eba755c0a1d0bcf306e9ed1742c956f7b86908878c86afa10d11eef1f'),  # arch/x86_64/efi_memory.c at 1b1cb5d7
}
RENAMES = {
    'bootinfo_ref': ['-Dmb2_parse=mb2_parse_reference', '-Dboot_framebuffer=boot_framebuffer_reference',
                     '-Dboot_mmap_entry=boot_mmap_entry_reference', '-Dboot_reserve=boot_reserve_reference', '-Dresv_add=resv_add_reference'],
    'resv_ref': ['-Dresv_init=resv_init_reference', '-Dresv_add=resv_add_reference', '-Dresv_overlaps=resv_overlaps_reference'],
    'efi_memory_ref': ['-Defi_memory_exclude=efi_memory_exclude_reference', '-Defi_memory_firmware_frame=efi_memory_firmware_frame_reference',
                       '-Defi_memory_strerror=efi_memory_strerror_reference', '-Defi_memory_what=efi_memory_what_reference',
                       '-Dresv_add=resv_add_reference'],
}
SYMBOLS = {  # symbol -> (kind in nm, production C call sites required)
    'resv_init': ('T', 1), 'resv_add': ('T', 0), 'resv_overlaps': ('T', 1), 'boot_mmap_entry': ('T', 1), 'boot_reserve': ('T', 1),
    'efi_memory_exclude': ('T', 1), 'efi_memory_firmware_frame': ('T', 1), 'efi_memory_strerror': ('T', 1), 'efi_memory_what': ('R', 0),
}
RETIRED = ['bootinfo', 'resv', 'efi_memory']
# (name, file, old, new, occurrences): each breaks one decision of the accepted C and must be caught.
MUTANTS = [
    ('page-zero-boundary', 'bootinfo_ref', 'if (page_down(start) < PAGE0_END)', 'if (page_down(start) <= PAGE0_END)', 1),
    ('legacy-hole-start', 'bootinfo_ref', 'LEGACY_HOLE, LOW_MEM_END)', 'LEGACY_HOLE + PAGE_SIZE, LOW_MEM_END)', 1),
    ('kernel-last-page', 'bootinfo_ref', 'page_down(kstart), page_up(kend))', 'page_down(kstart), page_down(kend))', 1),
    ('usable-containment', 'bootinfo_ref', 'start >= e->base && end <= e->base + e->length', 'start >= e->base && end < e->base + e->length', 1),
    ('empty-reserved-entry', 'bootinfo_ref', 'if (e->type == MB2_MMAP_AVAILABLE || e->length == 0)', 'if (e->type == MB2_MMAP_AVAILABLE)', 1),
    ('unavailable-overflow-extent', 'bootinfo_ref', 'e_end = UINT64_MAX;', 'e_end = e->base;', 1),
    ('kernel-below-1MiB', 'bootinfo_ref', 'if (kstart < LOW_MEM_END)', 'if (kstart <= LOW_MEM_END)', 1),
    ('module-overlaps-info', 'bootinfo_ref', 'if (overlap(m->start, m->end, bi->info_start, bi->info_end))', 'if (0)', 1),
    ('table-full-at-low-1MiB', 'bootinfo_ref', 'if (resv_add(t, 0, LOW_MEM_END, "low 1 MiB") != RESV_OK)', 'if (resv_add(t, 0, LOW_MEM_END, "low 1 MiB") == RESV_BAD_RANGE)', 1),
    ('mmap-entry-stride', 'bootinfo_ref', '(size_t)i * bi->mmap->entry_size', '(size_t)i * sizeof(struct mb2_mmap_entry)', 1),
    ('resv-round-up', 'resv_ref', '(end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)', 'end & ~(PAGE_SIZE - 1)', 1),
    ('resv-capacity', 'resv_ref', 't->count >= RESV_MAX', 't->count > RESV_MAX', 1),
    ('resv-overlap-strictness', 'resv_ref', 'start < t->r[i].end && t->r[i].start < end', 'start <= t->r[i].end && t->r[i].start < end', 1),
    ('efi-capacity', 'efi_memory_ref', '(uint32_t)(RESV_MAX - t->count)', '(uint32_t)(RESV_MAX - t->count - 1)', 1),
    ('efi-sink-sense', 'efi_memory_ref', 'EFI_MEMORY_WHAT) != RESV_OK', 'EFI_MEMORY_WHAT) == RESV_OK', 1),
    ('efi-provenance-identity', 'efi_memory_ref', 't->r[i].what == efi_memory_what &&', '', 1),
    ('efi-firmware-flag', 'efi_memory_ref', '.efi_firmware = bi->efi != 0,', '.efi_firmware = 1,', 1),
    ('efi-frame-end', 'efi_memory_ref', 'frame + PAGE_SIZE <= t->r[i].end', 'frame + PAGE_SIZE < t->r[i].end', 1),
    ('efi-error-text', 'efi_memory_ref', '"bad window"', '"bad  window"', 1),
    ('boot-services-flag', 'efi_memory_ref', '.boot_services_active = bi->efi_bs_active != 0,', '.boot_services_active = 0,', 1),
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


def require_converged(lines):
    if not lines or 'stopped on the plateau rule with every outcome seen at least 10 times' not in lines[0]:
        raise RuntimeError('boot reservation random phase did not converge')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--kernel-dir', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--rustc', default=os.environ.get('RUSTC'))
    a = p.parse_args()
    if not a.rustc:
        p.error('--rustc or RUSTC is required (the hosted overflow-checked build of the glue)')
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
    sources = {}
    for name, (path, sha) in REFS.items():
        sources[name] = (ROOT / path).read_text()
        if hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != sha:
            raise RuntimeError(f'{path} is not the accepted file')
    arch_mk = (ROOT / 'arch/x86_64/arch.mk').read_text()
    for stem in RETIRED:
        if (ROOT / f'arch/x86_64/{stem}.c').exists():
            raise RuntimeError(f'arch/x86_64/{stem}.c is still in the tree')
        if re.search(r'\b' + stem + r'\.o\b', arch_mk):
            raise RuntimeError(f'arch.mk still builds {stem}.o')
    glue = (ROOT / 'arch/x86_64/rust/boot_resv.rs').read_text()
    core = re.sub(r'//.*$', '', (ROOT / 'kernel/rust/boot/reserve.rs').read_text(), flags=re.M)
    if re.search(r'\bunsafe\b|\bextern\b|\bstd::|\balloc\b|\bpanic!|\bunwrap\(|\bexpect\(', core):
        raise RuntimeError('the reservation decisions must be safe, pure Rust without a panic path')
    for sym in SYMBOLS:
        exported = len(re.findall(r'pub (?:unsafe )?extern "C" fn ' + sym + r'\(', glue)) + len(re.findall(r'pub static ' + sym + r'\b', glue))
        if exported != 1:
            raise RuntimeError(f'{sym} must be exported exactly once, found {exported}')
    result['references'] = {n: sha for n, (_, sha) in REFS.items()}
    result['retired_c_files'] = [f'arch/x86_64/{s}.c' for s in RETIRED]
    # 2. production ownership
    archive_nm = subprocess.check_output(['nm', str(archive)], text=True)
    vos = kdir / 'vos.bin'
    vos_nm = subprocess.check_output(['nm', str(vos)], text=True)
    kmap = (kdir / 'kernel.map').read_text()
    dis = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(vos)], text=True)
    for stem in RETIRED:
        if (kdir / f'{stem}.o').exists():
            raise RuntimeError(f'{stem}.o was built')
    owned = {}
    for sym, (kind, calls_needed) in SYMBOLS.items():
        if not re.search(rf'^[0-9a-f ]+ {kind} {sym}$', archive_nm, re.M):
            raise RuntimeError(f'the archive does not define {sym}')
        m = re.search(rf'^([0-9a-f]+) {kind} {sym}$', vos_nm, re.M)
        if not m:
            raise RuntimeError(f'vos.bin does not define {sym}')
        section = r'\.text\.' if kind == 'T' else r'\.rodata\.'
        if not re.search(section + sym + r'\s*\n\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+\S*libvos_kernel\.a', kmap):
            raise RuntimeError(f'the link map does not take {sym} from the Rust archive')
        calls = len(re.findall(r'call\s+' + m.group(1) + r' <' + sym + '>', dis)) if kind == 'T' else None
        if calls is not None and calls < calls_needed:
            raise RuntimeError(f'no call site resolves to the Rust {sym}')
        owned[sym] = {'address': m.group(1), 'call_sites': calls, 'defined_by': 'libvos_kernel.a'}
    bad = [l for l in subprocess.check_output(['nm', '-C', str(vos)], text=True).splitlines() if re.search(r'core::(panicking|fmt)', l)]
    if bad:
        raise RuntimeError('panic/format symbols in vos.bin: ' + bad[0])
    result['ownership'] = {'symbols': owned, 'panic_fmt_symbols': 0, 'retired_objects_built': False}
    cc = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie', '-Iarch/x86_64']
    san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize=alignment', '-fno-omit-frame-pointer']
    tmp = Path(tempfile.mkdtemp(prefix='b1-'))
    try:
        refs = []
        for name, (path, _) in REFS.items():
            run(cc + san + RENAMES[name] + ['-c', path, '-o', out / f'{name}.o'], f'{name}-build.txt')
            refs.append(out / f'{name}.o')
        # 3. stack: the frames along each call chain (the entry point and the out-of-line functions of the Rust module it calls)
        def frame_of(binary, symbol):
            return frame_bytes(subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', f'--disassemble={symbol}', str(binary)], text=True))
        callees = re.findall(r'^[0-9a-f]+ t (\S*(?:boot_resv|reserve)\S*)$', vos_nm, re.M)
        kflags = ['-std=gnu11', '-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin',
                  '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx', '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone',
                  '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection', '-Iarch/x86_64']
        stack = {'out_of_line_callees': len(callees), 'ceiling': 512, 'increase_ceiling': 256}
        for name, (path, _) in REFS.items():
            run(['gcc'] + kflags + RENAMES[name] + ['-c', path, '-o', tmp / f'{name}-kernel.o'], f'{name}-kernel-build.txt')
        chains = {'boot_reserve': 'bootinfo_ref', 'efi_memory_exclude': 'efi_memory_ref', 'efi_memory_firmware_frame': 'efi_memory_ref',
                  'resv_add': 'resv_ref', 'resv_overlaps': 'resv_ref', 'boot_mmap_entry': 'bootinfo_ref'}
        for sym, ref in chains.items():
            rust_frame = frame_of(vos, sym) + (sum(frame_of(vos, c) for c in callees) if sym == 'boot_reserve' else 0)
            c_frame = frame_of(tmp / f'{ref}-kernel.o', sym + '_reference')
            if rust_frame > 512 or rust_frame > c_frame + 256:
                raise RuntimeError(f'stack {sym}: {rust_frame} B (C reference {c_frame} B) exceeds the budget')
            stack[sym] = {'rust_chain_bytes': rust_frame, 'c_reference_bytes': c_frame}
        result['stack'] = stack
        # 4. differential
        env = {'ASAN_OPTIONS': 'detect_leaks=0'}
        harness = ['tests/host/boot-resv-differential.c']
        run(cc + san + ['-Wl,--gc-sections', *harness, *refs, archive, '-o', out / 'differential'], 'differential-build.txt')
        d = run([out / 'differential'], 'differential.txt', env=env, timeout=1800).strip().splitlines()
        require_converged(d)
        result['differential_archive_build'] = d[1] if len(d) > 1 else d[0]
        result['differential_outcomes'] = d[2:]
        run([a.rustc, '--edition=2024', '-Dwarnings', '--crate-type=staticlib', '--crate-name', 'boot_resv_host', '-Cpanic=abort',
             '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '-Copt-level=1', 'tests/rust/boot_resv_host.rs', '-o', tmp / 'libboot_resv_host.a'],
            'host-rust-build.txt')
        run(cc + san + ['-Wl,--gc-sections', *harness, *refs, tmp / 'libboot_resv_host.a', '-o', tmp / 'differential-host'], 'differential-host-build.txt')
        h = run([tmp / 'differential-host'], 'differential-host.txt', env=env, timeout=1800).strip().splitlines()
        require_converged(h)
        result['differential_overflow_checked_host_build'] = h[1] if len(h) > 1 else h[0]
        # 5. negative controls: a broken reference must be detected (a mismatch, or a guard-page fault)
        controls = {}
        for name, which, old, new, count in MUTANTS:
            if sources[which].count(old) != count or old == new:
                raise RuntimeError(f'negative control {name}: anchor occurs {sources[which].count(old)} times, expected {count}')
            (tmp / 'mutant.c').write_text(sources[which].replace(old, new))
            objs = []
            for ref in REFS:
                if ref == which:
                    subprocess.run(cc + ['-O1'] + RENAMES[ref] + ['-c', tmp / 'mutant.c', '-o', tmp / 'mutant.o'], cwd=ROOT, check=True, capture_output=True)
                    objs.append(tmp / 'mutant.o')
                else:
                    subprocess.run(cc + ['-O1'] + RENAMES[ref] + ['-c', REFS[ref][0], '-o', tmp / f'{ref}-plain.o'], cwd=ROOT, check=True, capture_output=True)
                    objs.append(tmp / f'{ref}-plain.o')
            subprocess.run(cc + ['-O1', '-Wl,--gc-sections', *harness, *objs, archive, '-o', tmp / 'mutant-differential'], cwd=ROOT, check=True, capture_output=True)
            r = subprocess.run([tmp / 'mutant-differential'], cwd=tmp, capture_output=True, text=True, timeout=900)
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

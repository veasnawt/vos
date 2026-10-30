#!/usr/bin/env python3
"""USB.2-C4.1 hosted measurements of the initialization protocol model, for a future kernel budget.

The model is compiled on its own with the kernel's Rust flags (arch/x86_64/arch.mk KERNEL_RUST_FLAGS:
x86_64-unknown-none, panic=abort, opt-level=2, kernel code model, static relocation) into an archive
that is never linked into any kernel. It reports code and read-only data bytes (whole archive and the
model's own symbols), the largest stack frames, and the sizes of the state types. These are
hosted-object measurements, not kernel link measurements: they say how much code the model costs if a
later milestone links it, and nothing else. The kernel image is not touched by this script.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / 'kernel/rust/usb/xhci_init'


def sh(argv, **kw):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, check=True, cwd=ROOT, **kw).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'kernel_target': 'x86_64-unknown-none', 'linked_into_kernel': False}
    flags = ['--edition=2024', '--target=x86_64-unknown-none', '--crate-name', 'vos_xhci_init', '--crate-type=staticlib',
             '-D', 'warnings', '-C', 'panic=abort', '-C', 'opt-level=2', '-C', 'target-cpu=x86-64', '-C', 'code-model=kernel',
             '-C', 'relocation-model=static']
    report['rustc'] = sh([args.rustc, '-vV']).strip().splitlines()[0:3]
    # The model is built as an rlib with the kernel flags; a tiny anchor staticlib references the API so
    # the archive keeps the model's code (a staticlib only keeps what is reachable). The anchor holds no
    # state and no unsafe code; black_box stops the compiler from folding the call away.
    krlib = out / 'libvos_xhci_init_kernelflags.rlib'
    sh([args.rustc] + [f for f in flags if f != '--crate-type=staticlib'] + ['--crate-type=rlib', '--cap-lints=allow', MODEL / 'lib.rs', '-o', krlib])
    stub = out / 'anchor.rs'
    stub.write_text("""#![no_std]
use core::hint::black_box;
use vos_xhci_init::{Machine, Operation, Phase, Reason, ReleasePolicy, Rule};
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! { loop {} }
#[unsafe(no_mangle)]
pub extern "C" fn vos_measure_apply(code: u32) -> u32 {
    let mut m = black_box(Machine::new(ReleasePolicy::Never));
    let op = match code { 0 => Operation::SetBusMaster, 1 => Operation::SetRunStop, _ => Operation::Cancel };
    match black_box(&mut m).apply(op) { Ok(a) => a.why as u32, Err(r) => r as u32 + 1000 }
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_measure_names(i: u32) -> usize {
    Reason::ALL.get(i as usize).map_or(0, |r| r.name().len()) + Rule::ALL.len() + Phase::ALL.len()
}
""")
    archive = out / 'libvos_xhci_init_measure.a'
    stub_flags = [f if f != 'vos_xhci_init' else 'vos_measure_anchor' for f in flags]
    sh([args.rustc] + stub_flags + ['--cap-lints=allow', '--extern', f'vos_xhci_init={krlib}', stub, '-o', archive])
    # size per section over the archive members
    objs = out / 'objs'
    objs.mkdir()
    # Extract only the model's own members: the archive also bundles Rust core and compiler_builtins, which
    # are large and are not measured here.
    members = [m for m in sh(['ar', 't', archive]).split() if not (m.startswith('core-') or 'compiler_builtins' in m)]
    report['archive_members_measured'] = members
    subprocess.run(['ar', 'x', str(archive)] + members, cwd=objs, check=True)
    totals = {'.text': 0, '.rodata': 0, '.data': 0, '.bss': 0}
    per_member = {}
    for o in sorted(objs.glob('*.o')):
        if 'compiler_builtins' in o.name or o.name.startswith('core-'):
            continue
        sec = {}
        for line in sh(['size', '-A', o]).splitlines():
            m = re.match(r'^(\.\S+)\s+(\d+)\s+', line)
            if m:
                name = m.group(1)
                key = '.text' if name.startswith('.text') else '.rodata' if name.startswith('.rodata') else \
                    '.data' if name.startswith('.data') else '.bss' if name.startswith('.bss') else None
                if key:
                    sec[key] = sec.get(key, 0) + int(m.group(2))
        per_member[o.name] = sec
        for k, v in sec.items():
            totals[k] += v
    report['sections_all_members_bytes'] = totals
    report['members'] = per_member
    # symbol sizes (model code only)
    nm = sh(['nm', '-S', '--size-sort', '-C', archive])
    syms = []
    for line in nm.splitlines():
        m = re.match(r'^[0-9a-f]+ ([0-9a-f]+) ([tTrR]) (.+)$', line)
        if m and 'vos_xhci_init' in m.group(3) or (m and 'vos_measure' in m.group(3)):
            syms.append((int(m.group(1), 16), m.group(2), m.group(3)))
    syms.sort(reverse=True)
    report['model_symbols_text_bytes'] = sum(s for s, t, _ in syms if t in 'tT')
    report['model_symbols_rodata_bytes'] = sum(s for s, t, _ in syms if t in 'rR')
    report['largest_symbols'] = [{'bytes': s, 'kind': t, 'name': n[:120]} for s, t, n in syms[:12]]
    # stack frames: the largest `sub $N,%rsp` per function
    dis = sh(['objdump', '-d', '--no-show-raw-insn', '-C', archive])
    frames, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            cur = m.group(1)
            continue
        m = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', line)
        if m and cur:
            frames[cur] = max(frames.get(cur, 0), int(m.group(1), 16))
    modelframes = {k: v for k, v in frames.items() if 'vos_xhci_init' in k or 'vos_measure' in k}
    top = sorted(modelframes.items(), key=lambda kv: -kv[1])[:6]
    report['largest_frames_bytes'] = [{'bytes': v, 'function': k[:110]} for k, v in top]
    report['max_frame_bytes'] = top[0][1] if top else 0
    # state type sizes from a hosted probe
    probe = out / 'sizes.rs'
    probe.write_text('use vos_xhci_init::*;\nfn main() {\n  println!("{{\\"Machine\\":{},\\"Operation\\":{},\\"Accepted\\":{},\\"Facts\\":{},\\"Reason\\":{},\\"Rule\\":{}}}",\n'
                     '    core::mem::size_of::<Machine>(), core::mem::size_of::<Operation>(), core::mem::size_of::<Accepted>(),\n'
                     '    core::mem::size_of::<Facts>(), core::mem::size_of::<Reason>(), core::mem::size_of::<Rule>());\n}\n')
    rlib = out / 'libvos_xhci_init.rlib'
    sh([args.rustc, '--edition=2024', '--crate-name', 'vos_xhci_init', '--crate-type=rlib', '-Copt-level=2', MODEL / 'lib.rs', '-o', rlib])
    sh([args.rustc, '--edition=2024', '--extern', f'vos_xhci_init={rlib}', probe, '-o', out / 'sizes'])
    report['type_sizes_bytes'] = json.loads(sh([out / 'sizes']))
    report['source_lines'] = {p.name: len(p.read_text().splitlines()) for p in sorted(MODEL.glob('*.rs'))}
    report['source_lines_total'] = sum(report['source_lines'].values())
    report['bytes_per_source_line_text_rodata'] = round(
        (report['model_symbols_text_bytes'] + report['model_symbols_rodata_bytes']) / report['source_lines_total'], 2)
    (out / 'measurements.json').write_text(json.dumps(report, indent=1))
    print(json.dumps({k: report[k] for k in ('sections_all_members_bytes', 'model_symbols_text_bytes', 'model_symbols_rodata_bytes',
                                              'max_frame_bytes', 'type_sizes_bytes', 'source_lines_total',
                                              'bytes_per_source_line_text_rodata')}, indent=1))


if __name__ == '__main__':
    main()

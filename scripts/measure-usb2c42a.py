#!/usr/bin/env python3
"""USB.2-C4.2a hosted measurements of the backend contract, for a future kernel budget.

The model crate is compiled with the kernel's Rust flags (arch/x86_64/arch.mk KERNEL_RUST_FLAGS: x86_64-unknown-none,
panic=abort, opt-level=2, kernel code model, static relocation) as an rlib, and a tiny anchor crate instantiates the
generic `Session` over a stub backend and is compiled to a single object. Only objects are produced: there is no
staticlib and no bundled Rust `core` archive (the 15 MB the C4.1 measurement wasted). Nothing is linked into any
kernel. The numbers say how much code the model, the session over a stub backend and the DMA ledger cost; the real
backend (PCI, MMIO, DMA services) is not included and is not measured here.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / 'kernel/rust/usb/xhci_init'

ANCHOR = '''#![no_std]
use core::hint::black_box;
use vos_xhci_init::*;

struct Stub;
impl Backend for Stub {
    fn read(&mut self, _: Reg, _: u8) -> Result<RegRead, ReadErr> { black_box(Err(ReadErr { kind: ReadFault::Other, stamp: Stamp { seq: 0, tick: 0 }, session: 0 })) }
    fn read_identity(&mut self) -> ReadResult<Identity> { black_box(Err(ReadErr { kind: ReadFault::Other, stamp: Stamp { seq: 0, tick: 0 }, session: 0 })) }
    fn read_topology(&mut self) -> ReadResult<Topology> { black_box(Err(ReadErr { kind: ReadFault::Other, stamp: Stamp { seq: 0, tick: 0 }, session: 0 })) }
    fn read_platform(&mut self) -> ReadResult<PlatformFacts> { black_box(Err(ReadErr { kind: ReadFault::Other, stamp: Stamp { seq: 0, tick: 0 }, session: 0 })) }
    fn read_event(&mut self, _: u16, _: bool) -> ReadResult<Option<EventTrb>> { black_box(Err(ReadErr { kind: ReadFault::Other, stamp: Stamp { seq: 0, tick: 0 }, session: 0 })) }
    fn perform(&mut self, _: &ModelEvent, _: WriteTicket) -> Result<Receipt, WriteErr> { black_box(Err(WriteErr { kind: WriteFault::Refused, stamp: Stamp { seq: 0, tick: 0 }, session: 0 })) }
    fn latch_terminal(&mut self) {}
    fn now(&self) -> u64 { black_box(0) }
}

#[unsafe(no_mangle)]
pub extern "C" fn vos_measure_session(code: u32) -> u32 {
    let mut s = black_box(Session::new(Stub, Machine::new(ReleasePolicy::Never), Config { crcr_reads_zero: false }));
    let r = match code { 0 => Req::Bind, 1 => Req::Qualify, 2 => Req::Status, 3 => Req::Reset, 4 => Req::Event, _ => Req::Op(Operation::SetBusMaster) };
    match black_box(&mut s).request(r) { Ok(d) => d.closed.map_or(0, |c| c as u32 + 1), Err(_) => 1000 }
}

#[unsafe(no_mangle)]
pub extern "C" fn vos_measure_ledger(code: u32) -> u32 {
    let spec = PoolSpec { device_limit: 1 << 30, ac64: false, scratchpads: 4, quarantine_cap: 4, release: ReleasePolicy::Never };
    let Ok(mut l) = Ledger::new(black_box(spec)) else { return 1 };
    let i = if code == 0 { Item::Array } else { Item::Buffer(code as u8) };
    let r = l.allocate(i, 0x40_0000 + u64::from(code) * 0x1000).and_then(|()| l.zero(i)).and_then(|()| l.prepare(i)).and_then(|()| l.release(i));
    r.map_or_else(|e| e as u32 + 10, |()| 0)
}
'''


def sh(argv, **kw):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, check=True, cwd=ROOT, **kw)


def sections(obj):
    sec = {}
    for line in sh(['size', '-A', obj]).stdout.splitlines():
        m = re.match(r'^(\.\S+)\s+(\d+)\s+', line)
        if m:
            name = m.group(1)
            key = '.text' if name.startswith('.text') else '.rodata' if name.startswith('.rodata') else \
                '.data' if name.startswith('.data') else '.bss' if name.startswith('.bss') else None
            if key:
                sec[key] = sec.get(key, 0) + int(m.group(2))
    return sec


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'kernel_target': 'x86_64-unknown-none', 'linked_into_kernel': False, 'artifacts': 'objects only; no staticlib'}
    report['rustc'] = sh([args.rustc, '-vV']).stdout.strip().splitlines()[0:3]
    flags = ['--edition=2024', '--target=x86_64-unknown-none', '-D', 'warnings', '-C', 'panic=abort', '-C', 'opt-level=2', '-C', 'target-cpu=x86-64',
             '-C', 'code-model=kernel', '-C', 'relocation-model=static', '--cap-lints=allow']
    rlib = out / 'libvos_xhci_init.rlib'
    sh([args.rustc] + flags + ['--crate-name', 'vos_xhci_init', '--crate-type=rlib', MODEL / 'lib.rs', '-o', rlib])
    (out / 'objs').mkdir()
    members = [m for m in sh(['ar', 't', rlib]).stdout.split() if m.endswith('.o')]
    subprocess.run(['ar', 'x', str(rlib)] + members, cwd=out / 'objs', check=True)
    model_obj = [out / 'objs' / m for m in members]
    anchor = out / 'anchor.rs'
    anchor.write_text(ANCHOR)
    anchor_obj = out / 'anchor.o'
    log = subprocess.run([args.rustc] + flags + ['-Z', 'print-type-sizes', '--crate-name', 'anchor', '--crate-type=rlib', '--emit=obj',
                         '--extern', f'vos_xhci_init={rlib}', anchor, '-o', anchor_obj], capture_output=True, text=True, cwd=ROOT)
    if log.returncode:
        raise SystemExit('anchor build failed:\n' + log.stderr[-1500:])
    report['model_object_members'] = [m for m in members]
    report['model_object_bytes'] = {}
    for o in model_obj:
        for k, v in sections(o).items():
            report['model_object_bytes'][k] = report['model_object_bytes'].get(k, 0) + v
    report['session_over_stub_backend_object_bytes'] = sections(anchor_obj)
    # symbols and stack frames across both objects
    syms, frames = [], {}
    for o in model_obj + [anchor_obj]:
        for line in sh(['nm', '-S', '--size-sort', '-C', o]).stdout.splitlines():
            m = re.match(r'^[0-9a-f]+ ([0-9a-f]+) ([tTrR]) (.+)$', line)
            if m:
                syms.append((int(m.group(1), 16), m.group(2), m.group(3)))
        cur = None
        for line in sh(['objdump', '-d', '--no-show-raw-insn', '-C', o]).stdout.splitlines():
            m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
            if m:
                cur = m.group(1)
                continue
            m = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', line)
            if m and cur:
                frames[cur] = max(frames.get(cur, 0), int(m.group(1), 16))
    # Static worst-case call chain over the model and the anchor: frame = sub rsp + pushed registers + return address.
    # Cross-section calls are relocations against `.text.<mangled>` section symbols, so the graph is built on the
    # mangled names and the demangled names (same order in `objdump -C`) are only used for display.
    graph, frame, pretty = {}, {}, {}
    hdr = re.compile(r'^[0-9a-f]+ <(.+)>:$')
    for o in model_obj + [anchor_obj]:
        plain = sh(['objdump', '-dr', '--no-show-raw-insn', o]).stdout.splitlines()
        heads = [m.group(1) for m in map(hdr.match, plain) if m]
        names = [m.group(1) for m in map(hdr.match, sh(['objdump', '-d', '--no-show-raw-insn', '-C', o]).stdout.splitlines()) if m]
        pretty.update(zip(heads, names))
        cur, pend = None, None
        for line in plain:
            m = hdr.match(line)
            if m:
                cur, pend = m.group(1), None
                graph.setdefault(cur, set())
                frame[cur] = 8
                continue
            if cur is None:
                continue
            if pend:
                # A call to another section is followed by its relocation; an in-section call carries its target.
                r = re.search(r'R_X86_64_(?:PLT32|PC32)\s+\.text\.(\S+?)(?:[-+]0x[0-9a-f]+)?$', line)
                if r:
                    graph[cur].add(r.group(1))
                    pend = None
                    continue
                if pend[1] != cur:
                    graph[cur].add(pend[1])
                pend = None
            if re.search(r'\bpush\s', line):
                frame[cur] += 8
            m = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', line)
            if m:
                frame[cur] += int(m.group(1), 16)
            m = re.search(r'\b(?:call|jmp)\s+[0-9a-f]+ <(.+?)(?:\+0x[0-9a-f]+)?>$', line)
            if m:
                pend = ('x', m.group(1))
    memo, onpath = {}, set()

    def depth(f):
        if f in memo:
            return memo[f]
        if f in onpath:
            return (0, [f + ' (recursion)'])
        onpath.add(f)
        best = (0, [])
        for c in graph.get(f, ()):
            if c in frame:
                d = depth(c)
                if d[0] > best[0]:
                    best = d
        onpath.discard(f)
        memo[f] = (frame.get(f, 0) + best[0], [f] + best[1])
        return memo[f]
    entry = [(depth(f), f) for f in graph if 'vos_measure_session' in pretty.get(f, f)]
    report['worst_call_chain_bytes'] = entry[0][0][0] if entry else None
    report['worst_call_chain'] = [pretty.get(n, n)[:100] for n in entry[0][0][1]] if entry else None
    # The anchor holds a Session by value; a kernel keeps it in static storage, so the chain that matters starts at the
    # session's own entry point.
    disp = [(depth(f), f) for f in graph if pretty.get(f, '').endswith('Session<anchor::Stub>>::dispatch')]
    report['worst_chain_from_dispatch_bytes'] = disp[0][0][0] if disp else None
    report['worst_chain_from_dispatch'] = [pretty.get(n, n)[:100] for n in disp[0][0][1]] if disp else None
    syms.sort(reverse=True)
    report['named_text_bytes'] = sum(s for s, t, _ in syms if t in 'tT')
    report['named_rodata_bytes'] = sum(s for s, t, _ in syms if t in 'rR')
    report['largest_symbols'] = [{'bytes': s, 'kind': t, 'name': n[:130]} for s, t, n in syms[:14]]
    model = {k: v for k, v in frames.items() if 'vos_measure' not in k}
    harness = {k: v for k, v in frames.items() if 'vos_measure' in k}
    top = sorted(model.items(), key=lambda kv: -kv[1])[:8]
    report['largest_frames_bytes'] = [{'bytes': v, 'function': k[:120]} for k, v in top]
    report['max_frame_bytes'] = top[0][1] if top else 0
    report['measurement_harness_frames_bytes'] = harness  # the anchor holds a Session/Ledger by value; a kernel would not
    sizes = {}
    for m in re.finditer(r'print-type-size type: `([^`]+)`: (\d+) bytes', log.stdout + log.stderr):
        name = m.group(1)
        if any(t in name for t in ('Session<', 'Machine', 'Ledger', 'Done', 'Refusal', 'Operation', 'Facts', 'RegRead', 'Stamped<', 'Identity', 'Receipt', 'WriteTicket', 'PoolSpec')):
            sizes[name.replace('vos_xhci_init::', '')] = int(m.group(2))
    report['type_sizes_bytes'] = dict(sorted(sizes.items()))
    lines = {p.name: len(p.read_text().splitlines()) for p in sorted(MODEL.glob('*.rs'))}
    report['source_lines'] = lines
    new = {k: v for k, v in lines.items() if k in ('backend.rs', 'session.rs', 'dma_model.rs')}
    report['c42a_added_source_lines'] = sum(new.values())
    total = report['model_object_bytes'].get('.text', 0) + report['session_over_stub_backend_object_bytes'].get('.text', 0)
    totalro = report['model_object_bytes'].get('.rodata', 0) + report['session_over_stub_backend_object_bytes'].get('.rodata', 0)
    report['model_plus_session_text_bytes'] = total
    report['model_plus_session_rodata_bytes'] = totalro
    (out / 'measurements.json').write_text(json.dumps(report, indent=1))
    print(json.dumps({k: report[k] for k in ('model_object_bytes', 'session_over_stub_backend_object_bytes', 'named_text_bytes',
                                              'named_rodata_bytes', 'max_frame_bytes', 'worst_chain_from_dispatch_bytes', 'worst_chain_from_dispatch', 'worst_call_chain_bytes', 'type_sizes_bytes', 'c42a_added_source_lines')}, indent=1))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""USB.2-C1 image, linkage and stack measurements (read-only on its inputs).

Distinguishes three quantities: input-section bytes summed from the GNU ld
map (actual code/data), page-rounded PT_LOAD segment sizes (what the loader
reserves), and symbol sizes. Stack frames: Rust from disassembly (pushes plus
`sub rsp`), C from GCC -fstack-usage of the same objects and flags.
"""
import argparse
import collections
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SECTIONS = ('.text', '.rodata', '.data', '.bss')


def map_sections(path):
    totals = dict.fromkeys(SECTIONS, 0)
    objects = collections.Counter()
    current = None
    for line in Path(path).read_text().splitlines():
        m = re.match(r'^(\.\w+)\s', line)
        if m:
            current = m[1] if m[1] in SECTIONS else None
            continue
        m = re.match(r'^ (\.[\w.$]+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', line)
        if current and m:
            size = int(m[3], 16)
            totals[current] += size
            obj = m[4].split('/')[-1]
            obj = 'rust' if 'libvos_kernel' in obj else obj
            objects[(current, obj)] += size
    return totals, objects


def segments(path):
    rows = []
    for line in subprocess.check_output(['readelf', '-lW', str(path)], text=True).splitlines():
        s = line.split()
        if s and s[0] == 'LOAD':
            rows.append({'vaddr': s[2], 'filesz': int(s[4], 16), 'memsz': int(s[5], 16), 'flags': ' '.join(s[6:-1])})
    return rows


def symbols(path):
    out = {}
    for line in subprocess.check_output(['nm', '-S', '--defined-only', str(path)], text=True).splitlines():
        s = line.split()
        if len(s) == 4:
            out[s[3]] = (int(s[0], 16), int(s[1], 16), s[2])
    return out


def frame(path, start, size):
    text = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', '-M', 'intel',
                                    f'--start-address={start:#x}', f'--stop-address={start + size:#x}',
                                    str(path)], text=True)
    pushes = sub = 0
    for line in text.splitlines():
        if re.search(r'\tpush\s', line):
            pushes += 1
        m = re.search(r'\tsub\s+rsp,(0x[0-9a-f]+)', line)
        if m:
            sub = max(sub, int(m[1], 16))
    return pushes * 8 + sub


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, required=True, help='baseline build directory')
    ap.add_argument('--candidate', type=Path, required=True, help='candidate build directory')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--budget', type=int, default=20480)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists():
        raise SystemExit('fresh output required')
    out.mkdir(parents=True)
    b, bo = map_sections(args.baseline / 'kernel.map')
    c, co = map_sections(args.candidate / 'kernel.map')
    delta = {k: c[k] - b[k] for k in SECTIONS}
    growth = delta['.text'] + delta['.rodata']
    per_object = {f'{s} {o}': co[(s, o)] - bo[(s, o)] for (s, o) in sorted(set(bo) | set(co))
                  if co[(s, o)] != bo[(s, o)]}
    bseg, cseg = segments(args.baseline / 'vos.bin'), segments(args.candidate / 'vos.bin')
    seg_delta = [{'filesz': y['filesz'] - x['filesz'], 'memsz': y['memsz'] - x['memsz'], 'flags': y['flags']}
                 for x, y in zip(bseg, cseg)]
    syms = symbols(args.candidate / 'vos.bin')
    interesting = re.compile(r'vos_xhci|xhci_qualify|qualify|capabilities|controller|ring|scope_install|devmap_scope')
    frames = {}
    for name, (addr, size, kind) in syms.items():
        if kind in 'Tt' and interesting.search(name) and size:
            frames[name] = frame(args.candidate / 'vos.bin', addr, size)
    controller = sorted(n for n in syms if '10controller' in n or '9xhci_link' in n or n == 'vos_xhci_controller_link_v1')
    # C stack usage from the same compile flags as the kernel objects.
    cflags = ['gcc', '-std=gnu11', '-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector',
              '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx', '-O2', '-Wall',
              '-Wextra', '-g', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only',
              '-fcf-protection=none', '-fno-stack-clash-protection', '-fstack-usage', '-c']
    c_stack = {}
    for src in ('arch/x86_64/xhci_qualify.c', 'arch/x86_64/devmap.c', 'arch/x86_64/usb_resources.c'):
        obj = out / (Path(src).stem + '.o')
        subprocess.run(cflags + [src, '-o', str(obj)], cwd=ROOT, check=True)
        for line in obj.with_suffix('.su').read_text().splitlines():
            loc, size, kind = line.split('\t')
            c_stack[loc.split(':')[-1] + ' (' + Path(src).name + ')'] = (int(size), kind)
    mapx = (args.candidate / 'kernel.map').read_text().splitlines()
    excerpt = [mapx[i + j] for i, l in enumerate(mapx)
               if re.search(r'\.text\.(vos_xhci_controller_link_v1|vos_xhci_qualify_v1)$', l.strip()) for j in range(3)]
    result = {
        'budget_text_rodata_bytes': args.budget,
        'input_sections_baseline': b, 'input_sections_candidate': c, 'input_section_delta': delta,
        'text_rodata_growth_bytes': growth, 'within_budget': growth <= args.budget,
        'bss_growth_bytes': delta['.bss'], 'bss_within_256': delta['.bss'] <= 256,
        'per_object_delta': per_object,
        'pt_load_baseline': bseg, 'pt_load_candidate': cseg, 'pt_load_delta': seg_delta,
        'pt_load_total_memsz_delta': sum(x['memsz'] for x in seg_delta),
        'linked_controller_symbols': controller,
        'controller_symbol_bytes': sum(syms[n][1] for n in controller),
        'kernel_map_excerpt': [l.replace(str(ROOT) + '/', '') for l in excerpt],
        'rust_stack_frames_bytes': dict(sorted(frames.items(), key=lambda kv: -kv[1])),
        'rust_max_frame_bytes': max(frames.values()) if frames else 0,
        'c_stack_usage_bytes': dict(sorted(c_stack.items(), key=lambda kv: -kv[1][0])),
        'c_max_frame_bytes': max(v[0] for v in c_stack.values()),
    }
    (out / 'measurements.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('text_rodata_growth_bytes', 'within_budget', 'bss_growth_bytes',
                                             'pt_load_total_memsz_delta', 'rust_max_frame_bytes',
                                             'c_max_frame_bytes', 'controller_symbol_bytes')}, indent=2))


if __name__ == '__main__':
    main()

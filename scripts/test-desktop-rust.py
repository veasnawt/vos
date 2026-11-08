#!/usr/bin/env python3
"""R3-G1: Rust desktop_requested()/desktop_compute() against the frozen accepted C (tests/reference/desktop-r3g1.c).
Differential (tests/host/desktop-differential.c) in a release build linked with the actual kernel archive and in an
ASan/UBSan build linked with an overflow-checked host build of the same Rust sources; identical transcript digests;
named mutation controls; the accepted N6 desktop and pointer render host tests against the Rust decisions; symbol
ownership, panic freedom, call targets and entry frames in the linked kernel. Fresh confined output only."""
import argparse, hashlib, json, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = 'kernel/rust/gfx/desktop.rs'; GLUE = 'arch/x86_64/rust/desktop.rs'
FUNCS = ('desktop_requested', 'desktop_compute')
COVERAGE = (563445, 23483, 890761, 263604)   # deterministic case counts: requested (accepted), compute (accepted)
MUTATIONS = [  # (name, file, exact text, replacement): each must change the differential outcome
 ('width-lower-bound', CORE, 'width.wrapping_sub(MIN_WIDTH)', 'width.wrapping_sub(MIN_WIDTH + 1)'),
 ('height-lower-bound', CORE, 'height.wrapping_sub(MIN_HEIGHT)', 'height.wrapping_sub(MIN_HEIGHT - 1)'),
 ('width-upper-bound', CORE, '> MAX_DIM - MIN_WIDTH', '>= MAX_DIM - MIN_WIDTH'),
 ('height-upper-bound', CORE, '> MAX_DIM - MIN_HEIGHT', '>= MAX_DIM - MIN_HEIGHT'),
 ('max-dim-value', CORE, 'MAX_DIM: u32 = 8192', 'MAX_DIM: u32 = 8191'),
 ('min-width-value', CORE, 'MIN_WIDTH: u32 = 640', 'MIN_WIDTH: u32 = 639'),
 ('min-height-value', CORE, 'MIN_HEIGHT: u32 = 480', 'MIN_HEIGHT: u32 = 481'),
 ('x', CORE, 'x: 24,', 'x: 23,'),
 ('y', CORE, 'y: 44,', 'y: 45,'),
 ('w', CORE, 'width.wrapping_sub(48)', 'width.wrapping_sub(47)'),
 ('h', CORE, 'height.wrapping_sub(104)', 'height.wrapping_sub(103)'),
 ('text-x', CORE, 'text_x: 36,', 'text_x: 37,'),
 ('text-y', CORE, 'text_y: 84,', 'text_y: 83,'),
 ('text-w', CORE, 'width.wrapping_sub(72)', 'width.wrapping_sub(71)'),
 ('text-h', CORE, 'height.wrapping_sub(156)', 'height.wrapping_sub(155)'),
 ('token', CORE, 'b"vos.desktop=1"', 'b"vos.desktop=0"'),
 ('no-tab-separator', CORE, "byte == b' ' || byte == b'\\t'", "byte == b' '"),
 ('newline-separator', CORE, "byte == b' ' || byte == b'\\t'", "byte == b' ' || byte == b'\\t' || byte == b'\\n'"),
 ('leading-tab-kept', CORE, '        while separator(c) {', "        while c == b' ' {"),
 ('prefix-match', CORE, 'if end == 0 || separator(end) {', 'if end == 0 || separator(end) || end != 0 {'),
 ('end-of-line-field', CORE, 'if end == 0 || separator(end) {', 'if separator(end) {'),
 ('short-token-compare', CORE, 'while n < TOKEN.len() && at', 'while n + 1 < TOKEN.len() && at'),
 ('substring-restart', CORE, '        loop {\n            let c = at(p);\n            if c == 0 || separator(c) {\n                break;\n            }\n            p = p.wrapping_add(1);\n        }',
  '        if at(p) != 0 {\n            p = p.wrapping_add(1);\n        }'),
 ('null-requested', GLUE, '    if cmdline.is_null() {\n        return 0;', '    if cmdline.is_null() {\n        return 1;'),
 ('null-output', GLUE, '        Some(_) if out.is_null() => 0,', '        Some(_) if out.is_null() => 1,'),
 ('write-on-refusal', GLUE, '        None => 0,', '        None => {\n            unsafe { out.write(DesktopLayout { x: 0, y: 0, w: 0, h: 0, text_x: 0, text_y: 0, text_w: 0, text_h: 0 }) };\n            0\n        }'),
 ('swap-text-fields', GLUE, '                text_w: l.text_w,\n                text_h: l.text_h,', '                text_w: l.text_h,\n                text_h: l.text_w,'),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True); ap.add_argument('--kernel-dir', type=Path, required=True)
    ap.add_argument('--rustc', required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); archive = a.archive.resolve(); kdir = a.kernel_dir.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build') and archive.is_file() and (kdir / 'vos.bin').is_file(), 'fresh confined output')
    out.mkdir(parents=True)
    report = {'status': 'RUNNING', 'archive_sha256': sha(archive), 'kernel_sha256': sha(kdir / 'vos.bin'), 'commands': []}
    def run(cmd, name, expect=0, timeout=900, check=True):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = p.returncode
        if check: need(p.returncode == expect, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        ref = json.loads((ROOT / 'tests/reference/desktop-r3g1.json').read_text())
        for n, v in ref['sources_sha256'].items(): need(sha(ROOT / 'tests/reference' / n) == v, 'frozen accepted source ' + n)
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        rename = ['-D%s=ref_%s' % (f, f) for f in FUNCS + ('desktop_paint', 'desktop_bar')]
        def host_rust(tag, glue):
            root = out / ('root-%s.rs' % tag); lib = out / ('librust-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(glue)) + '] mod desktop;\nunsafe extern "C" {fn abort()->!;}\n'
                            '#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n'
                            '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
                 '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib
        def differential(tag, flags, rust, timeout=900):
            refo = out / ('ref-%s.o' % tag)
            run(['gcc', *flags, *warn, *rename, '-c', 'tests/reference/desktop-r3g1.c', '-o', refo], 'ref-%s-build.txt' % tag)
            b = out / ('diff-%s' % tag)
            run(['gcc', *flags, *warn, 'tests/host/desktop-differential.c', refo, rust, *link, '-o', b], 'diff-%s-build.txt' % tag)
            try: text = run([b], 'diff-%s.txt' % tag, check=False, timeout=timeout)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None
            m = re.search(r'DESKTOP DIFFERENTIAL requested=(\d+) \(true (\d+)\) compute=(\d+) \(accepted (\d+)\) mismatches=(\d+) digest=([0-9a-f]{16})', text)
            return text, (dict(requested=int(m[1]), requested_true=int(m[2]), compute=int(m[3]), compute_accepted=int(m[4]), mismatches=int(m[5]), digest=m[6]) if m else None)
        checked = host_rust('checked', ROOT / GLUE)
        report['differential'] = {}
        for tag, flags, rust in (('release', ['-O2'], archive), ('sanitizer', san, checked)):
            text, r = differential(tag, flags, rust)
            need(r and r['mismatches'] == 0 and 'MISMATCH' not in text, 'frozen C and Rust identical (%s)' % tag)
            need((r['requested'], r['requested_true'], r['compute'], r['compute_accepted']) == COVERAGE, 'exact coverage (%s): %s' % (tag, r))
            report['differential'][tag] = r
        need(report['differential']['release']['digest'] == report['differential']['sanitizer']['digest'], 'transcript independent of build mode')
        # the accepted N6 host tests (real screen.c/desktop.c renderer, guarded RAM) now link the Rust decisions
        cc = ['gcc', *warn, *link]
        run(cc + san + ['tests/host/desktop.c', 'arch/x86_64/desktop.c', 'arch/x86_64/font8x16.c', checked, '-o', out / 'desktop-host'], 'desktop-host-build.txt')
        report['desktop_host'] = run([out / 'desktop-host'], 'desktop-host.txt').strip()[-300:]
        run(cc + san + ['-DPOINTER_RENDER', 'tests/host/pointer.c', 'arch/x86_64/desktop.c', 'arch/x86_64/font8x16.c', checked, '-o', out / 'pointer-render'], 'pointer-render-build.txt')
        report['pointer_render'] = run([out / 'pointer-render'], 'pointer-render.txt').strip()[-300:]
        # named mutation controls on the safe core and the glue
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 20, 'at least 20 named mutation controls')
        for name, rel, old, new in MUTATIONS:
            d = out / 'mutants' / name
            for f in (CORE, GLUE): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            src = (d / rel).read_text(); need(src.count(old) == 1, 'unique mutation ' + name); (d / rel).write_text(src.replace(old, new))
            lib = host_rust('mutant-' + name, d / GLUE)
            text, r = differential('mutant-' + name, ['-O1'], lib, timeout=60)   # a mutant that hangs is a detected divergence
            detected = r is None or r['mismatches'] > 0
            need(detected, 'mutation not detected: ' + name)
            report['mutations'][name] = {'detected': True, 'mismatches': r['mismatches'] if r else ('hang (60 s)' if text == 'TIMEOUT' else 'abnormal exit')}
            shutil.rmtree(d)
        # linked kernel: ownership, panic freedom, call targets, frames
        k = kdir / 'vos.bin'
        syms = subprocess.run(['nm', k], capture_output=True, text=True, check=True).stdout
        dobj = subprocess.run(['nm', kdir / 'desktop.o'], capture_output=True, text=True, check=True).stdout
        need(all(re.search(r'^[0-9a-f]+ T %s$' % f, syms, re.M) for f in FUNCS), 'kernel defines both functions')
        need(all(re.search(r'^\s+U %s$' % 'desktop_compute', dobj, re.M) for _ in [0]) and not re.search(r' [TtWw] desktop_(requested|compute)$', dobj, re.M), 'desktop.o defines neither (Rust owns them)')
        need(not re.search(r'core::panicking|_ZN4core9panicking|panic_fmt|panic_bounds_check', syms), 'no panic machinery in the kernel')
        kmap = (kdir / 'kernel.map').read_text()
        need(all(re.search(r'\.text\.%s\s*\n?\s*0x[0-9a-f]+\s+0x[0-9a-f]+ .*libvos_kernel\.a' % f, kmap) for f in FUNCS), 'link map takes both from the Rust archive')
        addr = {m[2]: int(m[1], 16) for m in re.finditer(r'^([0-9a-f]+) [Tt] (\S+)$', syms, re.M)}
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', k], capture_output=True, text=True, check=True).stdout
        report['kernel'] = {}
        for f in FUNCS:
            body = dis[dis.index('<%s>:\n' % f):]; body = body[:body.index('\n\n')]
            calls = sorted(set(re.findall(r'call\s+[0-9a-f]+ <([^>]+)>', body)))
            need(set(calls) <= {'memcmp'}, 'call targets of %s: %s' % (f, calls))
            pushes = len(re.findall(r'\tpush ', body)); sub = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', body)
            report['kernel'][f] = {'address': hex(addr[f]), 'calls': calls, 'frame_bytes': pushes * 8 + (int(sub[1], 16) if sub else 0)}
        # the frozen C's own entry frames, compiled with the kernel's flags
        kflags = ['-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx',
                  '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection']
        run(['gcc', '-std=gnu11', *kflags, '-Iarch/x86_64', '-c', 'tests/reference/desktop-r3g1.c', '-o', out / 'ref-kernel.o'], 'ref-kernel-build.txt')
        rdis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', out / 'ref-kernel.o'], capture_output=True, text=True, check=True).stdout
        for f in FUNCS:
            body = rdis[rdis.index('<%s>:\n' % f):]; body = body[:body.index('\n\n')]
            sub = re.search(r'sub\s+\$0x([0-9a-f]+),%rsp', body)
            report['kernel'][f]['frozen_c_frame_bytes'] = len(re.findall(r'\tpush ', body)) * 8 + (int(sub[1], 16) if sub else 0)
        report['source_sha256'] = {n: sha(ROOT / n) for n in [CORE, GLUE, 'arch/x86_64/desktop.c', 'arch/x86_64/desktop.h', 'tests/host/desktop-differential.c',
                                                              'tests/reference/desktop-r3g1.c', 'tests/reference/desktop-r3g1.json', 'scripts/test-desktop-rust.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256')}, indent=2)[:5000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

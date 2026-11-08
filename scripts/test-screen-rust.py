#!/usr/bin/env python3
"""R3-G2: Rust screen.c geometry and bounds decisions against the frozen accepted C (tests/reference/screen-r3g2.c).
A whole-module differential (tests/host/screen-differential.c) runs the frozen and the production screen.c side by side
over the same seeded operations on guarded framebuffers and VGA cells, in a release build linked with the actual kernel
archive and in an ASan/UBSan build linked with an overflow-checked, debug-asserted host build of the same Rust; the two
transcript digests and the exact coverage must match. The accepted console, desktop and pointer host tests run against
the Rust decisions; named mutation controls must each be detected; and the linked kernel is checked for Rust ownership,
no panic machinery, call-free decisions and entry frames. Fresh confined output only; per-mutant products are released."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = 'kernel/rust/gfx/screen.rs'; GLUE = 'arch/x86_64/rust/screen.rs'; DGLUE = 'arch/x86_64/rust/desktop.rs'; DCORE = 'kernel/rust/gfx/desktop.rs'
PUBLIC = ('screen_kind screen_describe screen_init screen_putc screen_erase_previous screen_begin_input screen_restart_log screen_desktop_begin '
          'screen_status screen_panic screen_fill screen_glyph screen_gui_available screen_gui_begin screen_gui_row screen_gui_end '
          'screen_pointer_begin screen_pointer_move screen_pointer_end desktop_paint desktop_bar').split()
SCENARIOS, PER = 420, 260                 # main differential
MUTANT_SCENARIOS = 140                    # each mutant (release, same seed prefix)
COVERAGE = 'SCREEN COVERAGE desktop=452 gui=146 inside=241 outside=1424 drawn=5254 input=61119 vga=19144 scroll=151 ring_wrap=1143 erase_back=616 init_fb=172 init_refused=68'
MUTATIONS = [  # (name, file, exact text, replacement): each must make the differential mismatch or fault
 # Equivalent and excluded: x <= width / y <= height. At x == width the clipped extent is 0, so nothing is drawn; the
 # only effect is a cursor hide/show pair that restores identical pixels.
 ('visible-last-column', CORE, 'x < width && y < height', 'x < width.wrapping_sub(1) && y < height'),
 ('visible-last-row', CORE, 'x < width && y < height', 'x < width && y < height.wrapping_sub(1)'),
 ('span-overrun', CORE, 'if len > room { room } else { len }', 'if len > room.wrapping_add(1) { room } else { len }'),
 ('glyph-right-edge', CORE, 'width - x < GLYPH_W', 'width - x < GLYPH_W - 1'),
 ('glyph-bottom-edge', CORE, 'height - y < GLYPH_H', 'height - y <= GLYPH_H'),
 ('glyph-replacement', CORE, "{ b'?' as u32 - FONT_FIRST }", '{ 0 }'),
 ('glyph-range', CORE, 'c < FONT_FIRST + FONT_GLYPHS', 'c <= FONT_FIRST + FONT_GLYPHS'),
 # Equivalent and excluded: any more permissive overlap test (<=, >=). A false hit only hides and re-shows the cursor,
 # which restores identical pixels; only a missed real overlap is observable.
 ('overlap-x', CORE, 'x < cx + CURSOR as u64', 'x + 1 < cx + CURSOR as u64'),
 ('overlap-w', CORE, 'x + w > cx', 'x + w > cx + 1'),
 ('overlap-h', CORE, 'y + h > cy', 'y + h > cy + 1'),
 ('cursor-size', CORE, 'pub const CURSOR: u32 = 32;', 'pub const CURSOR: u32 = 31;'),
 ('cursor-extent-unclipped', CORE, 'if room < CURSOR { room } else { CURSOR }', 'if room < CURSOR { CURSOR } else { CURSOR }'),
 ('cell-x', CORE, 'text_x.wrapping_add(cx.wrapping_mul(GLYPH_W))', 'text_x.wrapping_add(cx.wrapping_mul(GLYPH_W)).wrapping_add(1)'),
 ('cell-y-offset', CORE, 'row_y(log_y, cy).wrapping_add(1)', 'row_y(log_y, cy)'),
 ('row-height', CORE, 'r.wrapping_mul(LINE_H)', 'r.wrapping_mul(LINE_H - 1)'),
 ('put-printable', CORE, 'c < 0x20 || c > 0x7e', 'c < 0x20 || c > 0x7f'),
 ('put-wrap', CORE, '} else if cx >= cols {', '} else if cx > cols {'),
 ('put-newline', CORE, "if c == b'\\n' {\n        PUT_NEWLINE", "if c == b'\\r' {\n        PUT_NEWLINE"),
 ('input-next-row', CORE, 'if next < rows { Some(next) } else { None }', 'if next <= rows { Some(next) } else { None }'),
 ('ring-next', CORE, 'if next >= rows { 0 } else { next }', 'if next > rows { 0 } else { next }'),
 ('ring-gap', CORE, 'cy.wrapping_add(1).checked_rem(rows)', 'cy.checked_rem(rows)'),
 ('erase-start-guard', CORE, 'if input && cy == 0 {', 'if (input || !input) && cy == 0 {'),
 ('erase-step-back', CORE, 'cy = cy.wrapping_add(rows).wrapping_sub(1)', 'cy = cy.wrapping_add(rows).wrapping_sub(2)'),
 ('fb-cols', CORE, 'width.wrapping_sub(2 * MARGIN) / GLYPH_W', 'width.wrapping_sub(MARGIN) / GLYPH_W'),
 ('fb-rows', CORE, 'height.wrapping_sub(log_y).wrapping_sub(MARGIN) / LINE_H', 'height.wrapping_sub(log_y) / LINE_H'),
 ('text-cols', CORE, '    text_w / GLYPH_W\n', '    text_w / GLYPH_W + 1\n'),
 ('text-rows', CORE, '    text_h / LINE_H\n', '    text_h / LINE_H + 1\n'),
 ('cells-ok', CORE, '!(rows < 2 || cols < 8)', '!(rows < 2 || cols < 9)'),
 ('input-cols', CORE, 'if cols > INPUT_COLS { INPUT_COLS } else { cols }', 'if cols > INPUT_COLS + 1 { INPUT_COLS + 1 } else { cols }'),
 ('input-rows', CORE, 'if rows > INPUT_ROWS { INPUT_ROWS } else { rows }', 'if rows > INPUT_ROWS - 1 { INPUT_ROWS - 1 } else { rows }'),
 ('gui-room-width', CORE, 'clear_w >= GUI_MAX_W', 'clear_w > GUI_MAX_W + 512'),
 ('gui-room-height', CORE, 'clear_bottom >= log_y.wrapping_add(GUI_MAX_H)', 'clear_bottom >= log_y.wrapping_add(GUI_MAX_H + 300)'),
 ('gui-size', CORE, 'w > GUI_MAX_W || h > GUI_MAX_H', 'w >= GUI_MAX_W || h > GUI_MAX_H'),
 ('gui-x', CORE, 'clear_x.wrapping_add(clear_w.wrapping_sub(w) / 2)', 'clear_x.wrapping_add(clear_w.wrapping_sub(w) / 2 + 1)'),
 ('gui-y', CORE, '.wrapping_sub(h) / 2)', '.wrapping_sub(h) / 2 + 1)'),
 ('pointer-home', CORE, 'origin.wrapping_add(extent / 2)', 'origin.wrapping_add(extent / 2 + 1)'),
 ('pointer-axis-low', CORE, '    if v < 0 {\n        0', '    if v < 0 {\n        1'),
 ('pointer-axis-high', CORE, 'v >= limit as i64', 'v > limit as i64'),
 ('pointer-inside', CORE, 'if d >= extent { (extent.wrapping_sub(1), false) } else { (d, true) }', 'if d > extent { (extent.wrapping_sub(1), false) } else { (d, true) }'),
 ('pointer-local-clamp', CORE, '(extent.wrapping_sub(1), false)', '(extent, false)'),
 ('pointer-pack', CORE, 'ly.wrapping_shl(8)', 'ly.wrapping_shl(9)'),
 ('pointer-inside-and', CORE, 'if inside_x && inside_y { INSIDE }', 'if inside_x || inside_y { INSIDE }'),
 ('pointer-inside-flag', CORE, 'pub const INSIDE: u32 = 0x20000;', 'pub const INSIDE: u32 = 0x10000;'),
 ('choose-any-tag', CORE, '    if fb_tags != 0 {\n        return (SCREEN_NONE, Reason::Refused);', '    if fb_tags > 1 {\n        return (SCREEN_NONE, Reason::Refused);'),
 ('choose-bda-mode', CORE, '(bda_mode == 3 || bda_mode == 7)', '(bda_mode == 3)'),
 ('choose-bda-cols', CORE, 'bda_cols == 80', 'bda_cols >= 80'),
 ('choose-error-blocks', CORE, 'if !has_error && fb_kind == BOOT_FB_BGRX32 {', 'if fb_kind == BOOT_FB_BGRX32 {'),
 ('reason-text', GLUE, 'b"unusable framebuffer information\\0"', 'b"unusable framebuffer info\\0"'),
 ('refused-passthrough', GLUE, 'if fb_err.is_null() { R_UNUSABLE.as_ptr() } else { fb_err }', 'R_UNUSABLE.as_ptr()'),
 ('erase-sentinel', GLUE, 'None => u64::MAX }', 'None => 0 }'),
 ('input-scroll-sentinel', GLUE, 'None => -1 }', 'None => rows as i64 - 1 }'),
]
FUNCS = ['screen_choose'] + ['vos_rust_screen_' + n for n in ('visible span glyph_index overlaps cursor_extent cell_x row_y cell_y put_action input_next_row ring_next '
         'ring_gap erase fb_cols fb_rows text_cols text_rows cells_ok input_cols input_rows gui_room gui_size_ok gui_x gui_y pointer_home pointer_axis pointer_state').split()]
C_FRAMES = ('screen_fill', 'screen_glyph', 'screen_putc', 'screen_erase_previous', 'screen_begin_input', 'screen_init', 'screen_desktop_begin',
            'screen_gui_available', 'screen_gui_begin', 'screen_pointer_move', 'screen_pointer_begin', 'screen_pointer_end', 'screen_gui_row')
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
        ref = json.loads((ROOT / 'tests/reference/screen-r3g2.json').read_text())
        for n, v in ref['sources_sha256'].items(): need(sha(ROOT / 'tests/reference' / n) == v, 'frozen accepted source ' + n)
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64', '-Itests/host']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        stub = ['-Dboot_framebuffer=test_boot_framebuffer']
        def side(tag, flags, which):
            renames = [('-D%s=%s_%s' % (s, which, s)) for s in PUBLIC] + (['-Dscreen_choose=ref_screen_choose'] if which == 'ref' else [])
            src = '../reference/screen-r3g2.c' if which == 'ref' else '../../arch/x86_64/screen.c'
            o = out / ('%s-%s.o' % (which, tag))
            run(['gcc', *flags, *warn, *stub, *renames, '-DPFX=%s_' % which, '-DSCREEN_SRC="%s"' % src, '-c', 'tests/host/screen-diff-unit.c', '-o', o], '%s-%s-build.txt' % (which, tag))
            return o
        def host_rust(tag, d):
            root = out / ('root-%s.rs' % tag); lib = out / ('librust-%s.a' % tag)
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(d / GLUE)) + '] mod screen;\n#[path=' + json.dumps(str(d / DGLUE)) + '] mod desktop;\n'
                            'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                            '// host test only: precompiled core references the personality even with panic=abort\n'
                            '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
            run([a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
                 '-Cpanic=abort', root, '-o', lib], 'rust-%s-build.txt' % tag)
            return lib, root
        def differential(tag, flags, objs, rust, args=(), timeout=1800):
            b = out / ('diff-%s' % tag)
            run(['gcc', *flags, *warn, 'tests/host/screen-differential.c', *objs, rust, *link, '-o', b], 'diff-%s-build.txt' % tag)
            try: text = run([b, *map(str, args)], 'diff-%s.txt' % tag, check=False, timeout=timeout)
            except subprocess.TimeoutExpired: return 'TIMEOUT', None, b
            m = re.search(r'SCREEN DIFFERENTIAL scenarios=(\d+) ops=(\d+) checks=(\d+) mismatches=(\d+) digest=([0-9a-f]{16})', text)
            return text, (dict(scenarios=int(m[1]), ops=int(m[2]), checks=int(m[3]), mismatches=int(m[4]), digest=m[5]) if m else None), b
        fonts = {}
        for tag, flags in (('release', ['-O2']), ('sanitizer', san)):
            fonts[tag] = out / ('font-%s.o' % tag); run(['gcc', *flags, *warn, '-c', 'arch/x86_64/font8x16.c', '-o', fonts[tag]], 'font-%s-build.txt' % tag)
        checked, _ = host_rust('checked', ROOT)
        report['differential'] = {}
        for tag, flags, rust in (('release', ['-O2'], archive), ('sanitizer', san, checked)):
            objs = [side(tag, flags, 'ref'), side(tag, flags, 'cand'), fonts[tag]]
            text, r, _ = differential(tag, flags, objs, rust, (SCENARIOS, PER))
            need(r and r['mismatches'] == 0 and 'MISMATCH' not in text, 'frozen C and Rust screen.c identical (%s): %s' % (tag, text[-400:]))
            need(COVERAGE in text, 'exact coverage (%s)' % tag)
            report['differential'][tag] = r
        need(report['differential']['release']['digest'] == report['differential']['sanitizer']['digest'], 'transcript independent of build mode')
        report['coverage'] = COVERAGE
        # accepted host tests that compile screen.c now link the Rust decisions
        cc = ['gcc', *warn, *link]
        # screen.c's status bar calls desktop_bar (N6), so the accepted console test links desktop.c as well
        run(cc + san + ['tests/host/screen-input.c', 'arch/x86_64/desktop.c', 'arch/x86_64/font8x16.c', checked, '-o', out / 'screen-input'], 'screen-input-build.txt')
        report['screen_input'] = run([out / 'screen-input'], 'screen-input.txt').strip()[-300:]
        run(cc + san + ['tests/host/desktop.c', 'arch/x86_64/desktop.c', 'arch/x86_64/font8x16.c', checked, '-o', out / 'desktop-host'], 'desktop-host-build.txt')
        report['desktop_host'] = run([out / 'desktop-host'], 'desktop-host.txt').strip()[-300:]
        run(cc + san + ['-DPOINTER_RENDER', 'tests/host/pointer.c', 'arch/x86_64/desktop.c', 'arch/x86_64/font8x16.c', checked, '-o', out / 'pointer-render'], 'pointer-render-build.txt')
        report['pointer_render'] = run([out / 'pointer-render'], 'pointer-render.txt').strip()[-300:]
        # named mutation controls (release, the reference and candidate C objects are shared; only the Rust changes)
        report['mutations'] = {}; need(len({m[0] for m in MUTATIONS}) == len(MUTATIONS) >= 40, 'at least 40 named mutation controls')
        mobjs = [side('mut', ['-O1'], 'ref'), side('mut', ['-O1'], 'cand'), fonts['release']]
        def mutant(m):
            name, rel, old, new = m
            d = out / 'mutants' / name
            for f in (CORE, GLUE, DCORE, DGLUE): (d / f).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / f, d / f)
            src = (d / rel).read_text(); need(src.count(old) == 1, 'unique mutation ' + name); (d / rel).write_text(src.replace(old, new))
            lib, root = host_rust('mutant-' + name, d)
            text, r, b = differential('mutant-' + name, ['-O1'], mobjs, lib, (MUTANT_SCENARIOS, PER), timeout=600)
            shutil.rmtree(d)
            for f in (lib, root, b): f.unlink(missing_ok=True)
            return name, r, text
        # independent mutants run concurrently (same seeds and checks; only wall-clock time changes)
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, len(os.sched_getaffinity(0))))) as pool:
            results = list(pool.map(mutant, MUTATIONS))
        for name, r, text in results:
            need(r is None or r['mismatches'] > 0, 'mutation not detected: ' + name)
            report['mutations'][name] = {'detected': True, 'mismatches': r['mismatches'] if r else ('hang' if text == 'TIMEOUT' else 'abnormal exit (fault)')}
        # linked kernel: ownership, panic freedom, call targets, frames
        k = kdir / 'vos.bin'
        syms = subprocess.run(['nm', k], capture_output=True, text=True, check=True).stdout
        sobj = subprocess.run(['nm', kdir / 'screen.o'], capture_output=True, text=True, check=True).stdout
        need(all(re.search(r'^[0-9a-f]+ T %s$' % f, syms, re.M) for f in FUNCS), 'kernel defines every decision')
        need(not any(re.search(r' [TtWw] %s$' % f, sobj, re.M) for f in FUNCS), 'screen.o defines none (Rust owns them)')
        need(not re.search(r'core::panicking|_ZN4core9panicking|panic_fmt|panic_bounds_check', syms), 'no panic machinery in the kernel')
        kmap = (kdir / 'kernel.map').read_text()
        need(all(re.search(r'\.text\.%s\s*\n?\s*0x[0-9a-f]+\s+0x[0-9a-f]+ .*libvos_kernel\.a' % f, kmap) for f in FUNCS), 'link map takes every decision from the Rust archive')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', k], capture_output=True, text=True, check=True).stdout
        report['rust'] = {}
        for f in FUNCS:
            body = dis[dis.index('<%s>:\n' % f):]; body = body[:body.index('\n\n')]
            calls = sorted(set(re.findall(r'call\s+[0-9a-f]+ <([^>]+)>', body)))
            need(not calls, 'call-free decision %s: %s' % (f, calls))
            report['rust'][f] = frames(k, [f])[f]
        # C entry frames: production screen.o in the kernel vs the frozen C compiled with the kernel's flags
        kflags = ['-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-mno-sse', '-mno-mmx',
                  '-O2', '-m64', '-mcmodel=kernel', '-mno-red-zone', '-mgeneral-regs-only', '-fcf-protection=none', '-fno-stack-clash-protection']
        run(['gcc', '-std=gnu11', *kflags, '-Iarch/x86_64', '-c', 'tests/reference/screen-r3g2.c', '-o', out / 'ref-kernel.o'], 'ref-kernel-build.txt')
        fr, fc = frames(out / 'ref-kernel.o', C_FRAMES), frames(kdir / 'screen.o', C_FRAMES)
        report['c_frames'] = {f: {'frozen': fr[f], 'production': fc[f]} for f in C_FRAMES}
        report['c_frame_growth'] = {f: v for f, v in report['c_frames'].items() if v['frozen'] is not None and v['production'] is not None and v['production'] > v['frozen']}
        report['source_sha256'] = {n: sha(ROOT / n) for n in [CORE, GLUE, 'arch/x86_64/screen.c', 'arch/x86_64/screen.h', 'arch/x86_64/rust_kernel.h', 'tests/host/screen-differential.c',
                                                              'tests/host/screen-diff-unit.c', 'tests/host/screen-diff.h', 'tests/reference/screen-r3g2.c', 'tests/reference/screen-r3g2.json', 'scripts/test-screen-rust.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256', 'mutations')}, indent=2)[:6000])
    print('mutation controls detected: %d' % len(report['mutations']))
if __name__ == '__main__': main()

#!/usr/bin/env python3
"""R3-G2 native regression: the accepted baseline kernel and the candidate kernel (Rust screen.c geometry and bounds
decisions) boot the same image recipe under SeaBIOS and OVMF and run the same interaction script:
- the plain and the desktop console: real vsh commands until the text area scrolls, typed text erased with backspace;
- (with --gui-apps) the GUI pointer route: the cursor moved inside the surface, onto its edges and to every screen
  corner, plus a click.
After every step the framebuffer is captured through QMP (raw PPM for comparison, PNG for review). The two kernels must
produce pixel-identical screens at every step and the same desktop/screen serial markers. Fresh confined output;
explicit kernels and application ELFs."""
import argparse, hashlib, importlib.util, json, os, re, shutil, subprocess, tempfile, time
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), REPO / 'scripts' / name)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
H = load('test-usb-probe.py'); V = load('test-usb-resources-native.py')
sha, need = H.sha, H.need
PROMPT = b'user[vsh]: VOS> '
CONSOLE = [('help', 1)] * 16 + [('echo erase-test-abcdef', 0)]
def console_steps():
    steps = [('shot', 'prompt')]
    for i, (cmd, _) in enumerate(CONSOLE[:-1]):
        steps.append(('command', cmd))
        if i in (7, 15): steps.append(('shot', 'after-%d-commands' % (i + 1)))
    steps += [('type', 'echo erase-test-abcdef'), ('shot', 'typed'), ('keys', ['backspace'] * 7), ('shot', 'erased'),
              ('keys', ['ret']), ('prompt', None), ('shot', 'echoed')]
    return steps
# (shot name, expected cursor tip): at 1024x768 the 256x160 surface is centred at (384, 310); the cursor starts at its centre.
POINTER = [('shot', ('ready', (512, 390))), ('mouse', (12, -8)), ('shot', ('moved', (524, 382))), ('mouse', (0, 0, True)), ('mouse', (0, 0, False)),
           ('shot', ('clicked', (524, 382))), ('mouse', (-1200, -900)), ('shot', ('top-left', (0, 0))), ('mouse', (1200, 0)), ('shot', ('top-right', (1023, 0))),
           ('mouse', (0, 900)), ('shot', ('bottom-right', (1023, 767))), ('mouse', (-1200, 0)), ('shot', ('bottom-left', (0, 767))),
           ('mouse', (384, -384)), ('shot', ('surface-left-edge', (384, 383))), ('mouse', (255, 0)), ('shot', ('surface-right-edge', (639, 383)))]
STEP = 60   # largest relative motion per input event; larger single events lose motion on the emulated PS/2 path
def tip(ppm):
    """The cursor's arrow tip: the first outline pixel (0x101a2a) in reading order, or None."""
    head = ppm.split(b'\n', 3); w, h = map(int, head[1].split()); data = head[3]
    i = data.find(b'\x10\x1a\x2a')
    while i >= 0 and i % 3: i = data.find(b'\x10\x1a\x2a', i + 1)
    return None if i < 0 else ((i // 3) % w, (i // 3) // w)
# (name, firmwares, cmdline, gfxpayload, xres, yres, apps, steps)
def cases(gui):
    c = [('console-1024x768', ('bios', 'uefi'), 'vos.apps=1', '1024x768x32', 1024, 768, ('vsh', 'hello', 'rixie'), console_steps()),
         ('desktop-1024x768', ('bios', 'uefi'), 'vos.apps=1 vos.desktop=1', '1024x768x32', 1024, 768, ('vsh', 'hello', 'rixie'), console_steps()),
         ('desktop-640x480', ('bios',), 'vos.apps=1 vos.desktop=1', '640x480x32', 640, 480, ('vsh', 'hello', 'rixie'), console_steps()),
         ('console-640x400', ('bios',), 'vos.apps=1 vos.desktop=1', '640x400x32', 640, 400, ('vsh', 'hello', 'rixie'), console_steps())]
    if gui:
        c.append(('gui-pointer-1024x768', ('bios', 'uefi'), 'vos.apps=1 vos.desktop=1 vos.gui=1 vos.pointer=1', '1024x768x32', 1024, 768,
                  ('vsh', 'hello', 'rixie', 'gui-demo', 'pointer-demo'), POINTER))
    return c
def boot(folder, fw, iso, xres, yres, steps, ready):
    folder.mkdir(); serial = folder / 'serial.log'; sock = folder / 'qmp'; short = None
    if len(str(sock)) >= 100: short = Path(tempfile.mkdtemp(prefix='vq-')); sock = short / 'qmp'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', '-vga', 'none', '-device', 'VGA,edid=on,xres=%d,yres=%d' % (xres, yres), '-serial', 'file:' + str(serial),
            '-qmp', 'unix:' + str(sock) + ',server=on,wait=off', '-no-reboot', '-no-shutdown']
    if fw == 'uefi':
        variables = folder / 'OVMF_VARS_4M.fd'; shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
        argv += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd', '-drive', 'if=pflash,format=raw,unit=1,file=' + str(variables)]
    (folder / 'command.json').write_text(json.dumps(argv, indent=2) + '\n')
    shots = []
    with (folder / 'qemu.log').open('x') as log:
        process = subprocess.Popen(argv, cwd=REPO, stdout=log, stderr=log); q = None
        try:
            def data():
                need(process.poll() is None, 'QEMU exited ' + str(folder))
                b = serial.read_bytes() if serial.exists() else b''
                need(not any(x in b for x in (b'panic:', b'boot: check failed', b'boot: FAILED', b'native: vsh FAIL', b'pointer-demo: FAIL', b'gui: FAIL')), 'guest failure ' + str(folder))
                return b
            def wait(pred, seconds=60):
                end = time.monotonic() + seconds
                while True:
                    b = data()
                    if pred(b): return b
                    need(time.monotonic() < end, 'bounded marker deadline ' + str(folder)); time.sleep(.02)
            def settle():   # the serial log and the screen are quiet for 0.6 s
                last, since = data(), time.monotonic()
                while time.monotonic() - since < 0.6:
                    time.sleep(.1); b = data()
                    if b != last: last, since = b, time.monotonic()
            wait(lambda b: ready in b, 90); q = H.QMP(sock); settle()
            for kind, arg in steps:
                if kind == 'shot':
                    arg, expect = arg if isinstance(arg, tuple) else (arg, None)
                    n = len(shots); ppm = folder / ('%02d-%s.ppm' % (n, arg)); png = folder / ('%02d-%s.png' % (n, arg))
                    q.command('screendump', {'filename': str(ppm)}); q.command('screendump', {'filename': str(png), 'format': 'png'})
                    for _ in range(200):
                        if ppm.exists() and png.exists() and ppm.stat().st_size: break
                        time.sleep(.05)
                    blob = ppm.read_bytes(); need(blob.startswith(b'P6\n'), 'raw PPM screendump')
                    at = tip(blob) if expect else None
                    if expect: need(at == expect, '%s: cursor tip %s, expected %s' % (arg, at, expect))
                    shots.append(dict(step=arg, ppm_sha256=hashlib.sha256(blob).hexdigest(), png=png.name, png_sha256=sha(png), cursor_tip=at)); ppm.unlink()
                elif kind == 'command':
                    n = data().count(PROMPT); q.text(arg); q.key('ret'); wait(lambda b: b.count(PROMPT) > n); settle()
                elif kind == 'type': q.text(arg); settle()
                elif kind == 'keys':
                    for k in arg: q.key(k); time.sleep(.05)
                    settle()
                elif kind == 'prompt': n = data().count(PROMPT); wait(lambda b: b.count(PROMPT) >= n); settle()
                elif kind == 'mouse':
                    dx, dy = arg[0], arg[1]
                    while dx or dy:   # bounded steps so no motion is lost
                        sx = max(-STEP, min(STEP, dx)); sy = max(-STEP, min(STEP, dy)); dx -= sx; dy -= sy; e = []
                        if sx: e.append({'type': 'rel', 'data': {'axis': 'x', 'value': sx}})
                        if sy: e.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
                        q.command('input-send-event', {'events': e}); time.sleep(.04)
                    if len(arg) > 2: q.command('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': arg[2]}}]})
                    settle()
            b = data()
        finally:
            if q: q.close()
            process.kill(); process.wait(10)
            if short: shutil.rmtree(short, ignore_errors=True)
            elif sock.is_socket(): sock.unlink()
    markers = [l.decode(errors='replace').strip() for l in b.split(b'\n') if l.startswith((b'desktop:', b'screen: ', b'gui:'))]
    pointer = [l.decode(errors='replace').strip() for l in b.split(b'\n') if b'pointer-demo:' in l and b'x=' in l]
    return dict(shots=shots, markers=markers, pointer_events=pointer[-12:])
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline-kernel', type=Path, required=True); ap.add_argument('--candidate-kernel', type=Path, required=True)
    ap.add_argument('--apps-dir', type=Path, required=True, help='directory with vsh.elf, hello.elf, rixie.elf (and gui-demo.elf, pointer-demo.elf with --gui-apps)')
    ap.add_argument('--gui-apps', action='store_true', help='also run the GUI pointer route')
    ap.add_argument('--efi', type=Path, required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); need(not out.exists(), 'fresh output'); out.mkdir(parents=True)
    tmp = out / 'tmp'; tmp.mkdir()
    kernels = {'baseline': a.baseline_kernel.resolve(), 'candidate': a.candidate_kernel.resolve()}
    report = {'passed': False, 'kernels': {k: sha(v) for k, v in kernels.items()}, 'cases': []}
    need(report['kernels']['baseline'] != report['kernels']['candidate'], 'two different kernels')
    try:
        for name, fws, cmdline, gfx, xres, yres, apps, steps in cases(a.gui_apps):
            paths = {n: (a.apps_dir / (n + '.elf')).resolve() for n in apps}; report.setdefault('apps', {}).update({n: sha(p) for n, p in paths.items()})
            ready = b'pointer-demo: READY;' if 'pointer-demo' in apps else PROMPT
            for fw in fws:
                row = {'case': name, 'firmware': fw, 'cmdline': cmdline, 'gfxpayload': gfx}
                for which, kernel in kernels.items():
                    cfg = out / ('%s-%s-%s.cfg' % (name, fw, which))
                    cfg.write_text(((REPO / 'arch/x86_64/grub-uefi-head.cfg').read_text() if fw == 'uefi' else '') +
                                   'set timeout=0\nset default=0\nmenuentry "VOS R3-G2 screen regression" {\nset gfxpayload=%s,auto\n' % gfx +
                                   'multiboot2 /boot/vos.bin ' + cmdline + '\n' + ''.join('module2 /boot/%s.elf %s\n' % (n, n) for n in apps) + 'boot\n}\n')
                    expected = {'/boot/vos.bin': kernel, '/boot/grub/grub.cfg': cfg, **{'/boot/%s.elf' % n: p for n, p in paths.items()}}
                    iso = out / ('%s-%s-%s.iso' % (name, fw, which))
                    modules = Path('/usr/lib/grub/i386-pc') if fw == 'bios' else a.efi
                    V.run(['grub-mkrescue', '-d', modules, '-o', iso, '-graft-points', *[k + '=' + str(v) for k, v in expected.items()]],
                          out / ('%s-%s-%s-producer.log' % (name, fw, which)), dict(os.environ, TMPDIR=str(tmp), PYTHONDONTWRITEBYTECODE='1'))
                    V.validate(iso, expected, fw, out / ('%s-%s-%s-image.log' % (name, fw, which)))
                    row[which] = boot(out / ('%s-%s-%s' % (name, fw, which)), fw, iso, xres, yres, steps, ready)
                    iso.unlink()
                b, c = row['baseline'], row['candidate']
                need(b['markers'] == c['markers'], '%s %s identical desktop/screen markers' % (name, fw))
                need([(s['step'], s['cursor_tip']) for s in b['shots']] == [(s['step'], s['cursor_tip']) for s in c['shots']], '%s %s same steps and cursor positions' % (name, fw))
                diff = [s['step'] for s, t in zip(b['shots'], c['shots']) if s['ppm_sha256'] != t['ppm_sha256']]
                need(not diff, '%s %s pixel-identical screens at every step (differ: %s)' % (name, fw, diff))
                row['pixel_identical_steps'] = len(c['shots']); report['cases'].append(row); print(name, fw, 'PASS', len(c['shots']), 'steps', flush=True)
        report['passed'] = True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
if __name__ == '__main__': main()

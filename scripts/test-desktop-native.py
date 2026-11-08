#!/usr/bin/env python3
"""R3-G1 native regression: the accepted baseline kernel and the candidate kernel (Rust desktop_requested/desktop_compute)
boot the same image recipe under SeaBIOS and OVMF at several framebuffer modes and command lines. Each boot runs to the
real vsh prompt; the framebuffer is captured through QMP (raw PPM, plus a PNG for review) and the two kernels' screens must
be pixel-identical, with the same desktop decision on serial. Fresh confined output; explicit kernels and application ELFs."""
import argparse, hashlib, importlib.util, json, os, shutil, subprocess, tempfile, time
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), REPO / 'scripts' / name)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
H = load('test-usb-probe.py'); V = load('test-usb-resources-native.py')
sha, need = H.sha, H.need
APPS = ('vsh', 'hello', 'rixie')
SCENE = b'desktop: scene ready; kernel-owned BGRX surface, bounded console viewport'
FALLBACK = b'desktop: unavailable; existing console fallback'
# (name, firmwares, cmdline, gfxpayload, xres, yres, expected desktop decision); 640x400 is a real mode below the 640x480 minimum
CASES = [
    ('desktop-1024x768', ('bios', 'uefi'), 'vos.apps=1 vos.desktop=1', '1024x768x32', 1024, 768, 'scene'),
    ('desktop-800x600', ('bios', 'uefi'), 'vos.apps=1 vos.desktop=1', '800x600x32', 800, 600, 'scene'),
    ('desktop-640x480', ('bios', 'uefi'), 'vos.apps=1 vos.desktop=1', '640x480x32', 640, 480, 'scene'),
    ('desktop-tab-field', ('bios',), 'vos.apps=1\tvos.desktop=1', '1024x768x32', 1024, 768, 'scene'),
    ('not-requested', ('bios', 'uefi'), 'vos.apps=1', '1024x768x32', 1024, 768, 'none'),
    ('near-miss-token', ('bios',), 'vos.apps=1 vos.desktop=10 xvos.desktop=1', '1024x768x32', 1024, 768, 'none'),
    ('desktop-1280x800', ('uefi',), 'vos.apps=1 vos.desktop=1', '1280x800x32', 1280, 800, 'scene'),
    # OVMF offers no mode below 640x480 here (it falls back to 1280x800), so the native refusal path is SeaBIOS-only.
    ('below-minimum-640x400', ('bios',), 'vos.apps=1 vos.desktop=1', '640x400x32', 640, 400, 'fallback'),
]
def boot(folder, fw, iso, xres, yres):
    folder.mkdir(); serial = folder / 'serial.log'; sock = folder / 'qmp'
    short = None
    if len(str(sock)) >= 100:   # AF_UNIX path limit: a private short directory, removed after the boot
        short = Path(tempfile.mkdtemp(prefix='vq-')); sock = short / 'qmp'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', '-vga', 'none', '-device', 'VGA,edid=on,xres=%d,yres=%d' % (xres, yres), '-serial', 'file:' + str(serial),
            '-qmp', 'unix:' + str(sock) + ',server=on,wait=off', '-no-reboot', '-no-shutdown']
    if fw == 'uefi':
        variables = folder / 'OVMF_VARS_4M.fd'; shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
        argv += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                 '-drive', 'if=pflash,format=raw,unit=1,file=' + str(variables)]
    (folder / 'command.json').write_text(json.dumps(argv, indent=2) + '\n')
    with (folder / 'qemu.log').open('x') as log:
        process = subprocess.Popen(argv, cwd=REPO, stdout=log, stderr=log); q = None
        try:
            end = time.monotonic() + 90
            while True:
                need(process.poll() is None, 'QEMU exited ' + str(folder))
                b = serial.read_bytes() if serial.exists() else b''
                need(not any(x in b for x in (b'panic:', b'boot: check failed', b'boot: FAILED', b'native: vsh FAIL')), 'guest failure ' + str(folder))
                if b'user[vsh]: VOS> ' in b: break
                need(time.monotonic() < end, 'vsh prompt deadline ' + str(folder)); time.sleep(.02)
            time.sleep(1.0)   # let the prompt's screen echo settle; the screen is compared, not timed
            q = H.QMP(sock)
            q.command('screendump', {'filename': str(folder / 'screen.ppm')})
            q.command('screendump', {'filename': str(folder / 'screen.png'), 'format': 'png'})
            for _ in range(200):
                if (folder / 'screen.ppm').exists() and (folder / 'screen.png').exists(): break
                time.sleep(.05)
            b = serial.read_bytes()
        finally:
            if q: q.close()
            process.kill(); process.wait(10)
            if short: shutil.rmtree(short, ignore_errors=True)
            elif sock.is_socket(): sock.unlink()   # leave only regular evidence files behind
    ppm = (folder / 'screen.ppm').read_bytes(); need(ppm.startswith(b'P6\n'), 'raw PPM screendump')
    lines = [l for l in b.split(b'\n') if l.startswith((b'desktop:', b'screen: '))]
    decision = 'scene' if SCENE in b else 'fallback' if FALLBACK in b else 'none'
    return dict(ppm_sha256=hashlib.sha256(ppm).hexdigest(), png=str(folder / 'screen.png'), png_sha256=sha(folder / 'screen.png'), ppm_bytes=len(ppm),
                decision=decision, marker_lines=[l.decode(errors='replace').strip() for l in lines])
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline-kernel', type=Path, required=True); ap.add_argument('--candidate-kernel', type=Path, required=True)
    ap.add_argument('--apps-dir', type=Path, required=True, help='directory with vsh.elf, hello.elf and rixie.elf')
    ap.add_argument('--efi', type=Path, required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); need(not out.exists(), 'fresh output'); out.mkdir(parents=True)
    tmp = out / 'tmp'; tmp.mkdir()
    kernels = {'baseline': a.baseline_kernel.resolve(), 'candidate': a.candidate_kernel.resolve()}
    apps = {n: (a.apps_dir / (n + '.elf')).resolve() for n in APPS}
    report = {'passed': False, 'kernels': {k: sha(v) for k, v in kernels.items()}, 'apps': {n: sha(p) for n, p in apps.items()}, 'cases': []}
    need(report['kernels']['baseline'] != report['kernels']['candidate'], 'two different kernels')
    try:
        for name, fws, cmdline, gfx, xres, yres, expect in CASES:
            for fw in fws:
                row = {'case': name, 'firmware': fw, 'cmdline': cmdline, 'gfxpayload': gfx, 'expected': expect}
                for which, kernel in kernels.items():
                    cfg = out / ('%s-%s-%s.cfg' % (name, fw, which))
                    cfg.write_text(((REPO / 'arch/x86_64/grub-uefi-head.cfg').read_text() if fw == 'uefi' else '') +
                                   'set timeout=0\nset default=0\nmenuentry "VOS R3-G1 desktop regression" {\n' +
                                   'set gfxpayload=%s,auto\n' % gfx +
                                   'multiboot2 /boot/vos.bin ' + cmdline + '\n' + ''.join('module2 /boot/%s.elf %s\n' % (n, n) for n in APPS) + 'boot\n}\n')
                    expected = {'/boot/vos.bin': kernel, '/boot/grub/grub.cfg': cfg, **{'/boot/%s.elf' % n: p for n, p in apps.items()}}
                    iso = out / ('%s-%s-%s.iso' % (name, fw, which))
                    modules = Path('/usr/lib/grub/i386-pc') if fw == 'bios' else a.efi
                    V.run(['grub-mkrescue', '-d', modules, '-o', iso, '-graft-points', *[k + '=' + str(v) for k, v in expected.items()]],
                          out / ('%s-%s-%s-producer.log' % (name, fw, which)), dict(os.environ, TMPDIR=str(tmp), PYTHONDONTWRITEBYTECODE='1'))
                    V.validate(iso, expected, fw, out / ('%s-%s-%s-image.log' % (name, fw, which)))
                    row[which] = boot(out / ('%s-%s-%s' % (name, fw, which)), fw, iso, xres, yres)
                    iso.unlink()
                b, c = row['baseline'], row['candidate']
                need(b['decision'] == c['decision'] == expect, '%s %s desktop decision %s/%s' % (name, fw, b['decision'], c['decision']))
                need(b['marker_lines'] == c['marker_lines'], '%s %s identical desktop/screen serial markers' % (name, fw))
                need(b['ppm_sha256'] == c['ppm_sha256'], '%s %s pixel-identical screens' % (name, fw))
                row['pixel_identical'] = True; report['cases'].append(row); print(name, fw, 'PASS', c['decision'], flush=True)
        need(len(report['cases']) == sum(len(c[1]) for c in CASES), 'every case ran')
        report['passed'] = True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
if __name__ == '__main__': main()

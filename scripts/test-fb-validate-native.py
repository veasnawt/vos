#!/usr/bin/env python3
"""A3: show that the real production boot path enters the Rust boot_framebuffer, on Legacy BIOS and on UEFI (default, no-GOP,
640x480 and 1920x1080), in QEMU.

Each production ISO is booted with QEMU's gdbstub (`-S`, owned by this script). The independent observer in scripts/vos_gdbstub.py
sets a one-shot breakpoint at the vos.bin address of boot_framebuffer (the symbol that the link map takes from libvos_kernel.a),
lets the guest run and requires the trap to land with RIP equal to that address. It records the arguments (rdi = struct boot_info,
rsi = struct boot_fb), then traps again at the return address and reads the result: NULL and the struct boot_fb the Rust code
filled, which must match the `screen: framebuffer WxH BGRX at ..., pitch P` line the kernel then prints; or, on the no-GOP image,
the static message "no framebuffer information" and an all-zero struct. The observer never writes guest memory or registers. The
guest is then released and must finish the normal boot ("boot: checks passed"). QEMU only; no physical hardware. No case is retried.

usage: test-fb-validate-native.py --kernel-dir DIR --out DIR   (DIR holds vos.bin, vos.iso, vos-uefi.iso, vos-uefi-nogop.iso)
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from vos_gdbstub import GdbStub  # noqa: E402

OVMF_CODE, OVMF_VARS = '/usr/share/OVMF/OVMF_CODE_4M.fd', '/usr/share/OVMF/OVMF_VARS_4M.fd'
CASES = [('bios', 'vos.iso', False, []), ('uefi', 'vos-uefi.iso', True, []), ('uefi-nogop', 'vos-uefi-nogop.iso', True, []),
         ('uefi-640x480', 'vos-uefi.iso', True, ['-vga', 'none', '-device', 'VGA,edid=on,xres=640,yres=480']),
         ('uefi-1920x1080', 'vos-uefi.iso', True, ['-vga', 'none', '-device', 'VGA,edid=on,xres=1920,yres=1080'])]


class Fail(Exception):
    pass


def need(cond, what):
    if not cond:
        raise Fail(what)


def regs(g):
    text = g.command('g')
    vals = [int.from_bytes(bytes.fromhex(text[i:i + 16]), 'little') for i in range(0, 17 * 16, 16)]
    names = ['rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp'] + [f'r{n}' for n in range(8, 16)] + ['rip']
    return dict(zip(names, vals))


def run_one(out, kdir, name, iso_name, efi, extra, symbol):
    folder = out / name
    folder.mkdir()
    serial, sock = folder / 'serial.txt', folder / 'gdb'
    iso = kdir / iso_name
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', '-serial', f'file:{serial}', '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-no-reboot', '-no-shutdown', *extra]
    if efi:
        variables = folder / 'OVMF_VARS_4M.fd'
        shutil.copyfile(OVMF_VARS, variables)
        argv += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'case': name, 'iso': iso.name, 'symbol': 'boot_framebuffer', 'address': hex(symbol), 'passed': False}
    with (folder / 'qemu.txt').open('x') as log:
        proc = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        g = None
        try:
            end = time.monotonic() + 30
            while not sock.exists():
                need(proc.poll() is None and time.monotonic() < end, 'QEMU gdbstub did not appear')
                time.sleep(0.2)
            g = GdbStub(sock, timeout=300)
            need(g.stop_reason().startswith(('T', 'S')), 'the target starts stopped')
            g.insert(0, symbol, 1)
            need(g.cont().startswith(('T05', 'S05')), 'a breakpoint trap')
            r = regs(g)
            need(r['rip'] == symbol, f"RIP {r['rip']:#x} equals the Rust boot_framebuffer {symbol:#x}")
            row['entry'] = {'rip': hex(r['rip']), 'boot_info': hex(r['rdi']), 'boot_fb': hex(r['rsi'])}
            fb_tags = int.from_bytes(g.read(r['rdi'] + 1600, 4), 'little')
            ret = g.u64(r['rsp'])
            g.remove(0, symbol, 1)
            g.insert(0, ret, 1)
            need(g.cont().startswith(('T05', 'S05')), 'the return trap')
            r2 = regs(g)
            need(r2['rip'] == ret, 'the return trap is at the return address')
            raw = g.read(r['rsi'], 40)
            u64 = lambda o: int.from_bytes(raw[o:o + 8], 'little')  # noqa: E731
            u32 = lambda o: int.from_bytes(raw[o:o + 4], 'little')  # noqa: E731
            fb = {'kind': u32(0), 'addr': u64(8), 'size': u64(16), 'pitch': u32(24), 'width': u32(28), 'height': u32(32)}
            row['return'] = {'rip': hex(r2['rip']), 'rax': hex(r2['rax'])}
            row['fb_tags'] = fb_tags
            g.remove(0, ret, 1)
            if name == 'uefi-nogop':
                msg = g.read(r2['rax'], 27)
                need(r2['rax'] != 0 and msg == b'no framebuffer information\0', 'the static message "no framebuffer information"')
                need(not any(fb.values()) and fb_tags == 0, 'an all-zero struct boot_fb and no recorded tag')
                row['message'] = msg[:-1].decode()
            else:
                need(r2['rax'] == 0 and fb['kind'] == 1, 'NULL return and a BGRX32 struct boot_fb')
                need(fb['size'] == fb['pitch'] * fb['height'] and fb['pitch'] >= fb['width'] * 4, 'consistent size and pitch')
            row['boot_fb'] = {k: (hex(v) if k == 'addr' else v) for k, v in fb.items()}
            g._send('c')  # release the guest; the stop reply never comes while it runs
            end = time.monotonic() + 240
            text = ''
            while time.monotonic() < end:
                need(proc.poll() is None, 'QEMU exited early')
                text = serial.read_text(errors='replace') if serial.exists() else ''
                need('panic:' not in text, 'guest panic')
                if 'boot: checks passed' in text:
                    break
                time.sleep(1)
            need('boot: checks passed' in text, 'the normal boot completes after the Rust validation')
            if name == 'uefi-nogop':
                need('screen: none, serial only: UEFI boot without framebuffer information' in text, 'the kernel reports the no-framebuffer screen')
            else:
                m = re.search(r'screen: framebuffer (\d+)x(\d+) BGRX at (0x[0-9a-f]+), pitch (\d+)', text)
                need(m and (int(m[1]), int(m[2]), int(m[3], 16), int(m[4])) == (fb['width'], fb['height'], fb['addr'], fb['pitch']),
                     'the kernel prints the framebuffer the Rust code validated')
                row['serial'] = m[0]
            row['passed'] = True
        except Fail as e:
            row['failure'] = str(e)
        finally:
            if g:
                g.close()
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
    (folder / 'result.json').write_text(json.dumps(row, indent=1) + '\n')
    print(json.dumps(row), flush=True)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out, kdir = a.out.resolve(), a.kernel_dir.resolve()
    if out.exists():
        ap.error('output already exists: preserve it and choose a fresh path')
    out.mkdir(parents=True)
    nm = subprocess.check_output(['nm', str(kdir / 'vos.bin')], text=True)
    m = re.search(r'^([0-9a-f]+) T boot_framebuffer$', nm, re.M)
    need(m, 'vos.bin defines boot_framebuffer')
    rows = [run_one(out, kdir, *case, int(m[1], 16)) for case in CASES]
    ok = all(r['passed'] for r in rows)
    (out / 'result.json').write_text(json.dumps({'passed': ok, 'cases': rows}, indent=1) + '\n')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

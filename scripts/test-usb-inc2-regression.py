#!/usr/bin/env python3
"""USB input bring-up, Increment 2: regression boots of the paths the survey v2 change shares with earlier USB work.

One Legacy BIOS ISO carries four GRUB entries that are chosen by QMP key presses at the menu: normal boot, the C1 read-only
qualification (a BAR lease through the devmap scopes), the C2 DMA ownership exercise (the DMA scope in the same registry
and audit that gained the read-only flag), and the survey + USB.1 probe conflict (the kernel.c ordering change). Each must
finish its own boot with the earlier result lines and without `usb:` survey output where none is expected. UEFI boots of
the default image are covered by test-usb-survey-native.py. QEMU only; no host disk, no network, no physical hardware.
No case is retried. Inputs are the kernel under test and the existing hello application, both hashed in result.json.
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ENTRIES = [
    ('n', 'normal', ''),
    ('q', 'qualify', 'vos.usb2-qualify=1'),
    ('d', 'dma', 'vos.usb2-residue=1 vos.usb2c2-dma=1'),
    ('c', 'conflict', 'vos.usb-survey=1 vos.usb_probe=1'),
]
XHCI = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0']


class Fail(Exception):
    pass


def need(cond, what):
    if not cond:
        raise Fail(what)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def qmp(path, command, **args):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(15)
    s.connect(str(path))
    f = s.makefile('rw')
    f.readline()
    for msg in ({'execute': 'qmp_capabilities'}, {'execute': command, 'arguments': args}):
        f.write(json.dumps(msg) + '\n')
        f.flush()
        reply = json.loads(f.readline())
    s.close()
    need('error' not in reply, f'{command}: {reply}')


def boot(out, iso, index, key):
    folder = out / key
    folder.mkdir()
    serial, sock = folder / 'serial.txt', folder / 'qmp'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', *XHCI, '-serial', f'file:{serial}', '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    with (folder / 'qemu.txt').open('x') as log:
        proc = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and 'VOS-REGRESSION-MENU' not in (serial.read_text(errors='replace') if serial.exists() else ''):
                need(proc.poll() is None, 'QEMU exited early')
                time.sleep(0.5)
            need('VOS-REGRESSION-MENU' in serial.read_text(errors='replace'), 'the GRUB menu did not appear')
            time.sleep(1)
            for _ in range(index):
                qmp(sock, 'send-key', keys=[{'type': 'qcode', 'data': 'down'}])
                time.sleep(0.3)
            qmp(sock, 'send-key', keys=[{'type': 'qcode', 'data': 'ret'}])
            deadline = time.monotonic() + 120
            text = ''
            while time.monotonic() < deadline:
                need(proc.poll() is None, 'QEMU exited early')
                text = serial.read_text(errors='replace')
                need('panic:' not in text, 'guest panic')
                if 'boot: checks passed' in text:
                    break
                time.sleep(1)
            need('boot: checks passed' in text, 'no "boot: checks passed"')
            return text
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()


def check(key, text):
    boot_text = text[text.index('boot: kernel command line'):]
    need('INTEGRITY' not in text and 'FAILURE' not in text, 'no integrity failure')
    if key == 'n':
        need('boot: kernel command line ""' in text, 'the normal entry has an empty command line')
        need('usb' not in boot_text.replace('usb2', ''), 'no usb output in a normal boot')
    elif key == 'q':
        need('usb2c1: QUALIFIED: profile qualified' in boot_text, 'C1 qualification result')
        need('usb2c1: resources restored; no PCI/xHCI writes' in boot_text, 'C1 resources restored')
        need('usb: ' not in boot_text, 'no survey output')
    elif key == 'd':
        need('usb2c2: PASS' in boot_text, 'C2 DMA ownership exercise passed')
        need('usb: ' not in boot_text, 'no survey output')
    elif key == 'c':
        need('usb: survey is exclusive with the other vos.usb options; not run' in boot_text, 'survey refused beside the probe')
        need('usb: USB.1 read-only probe' in boot_text, 'the USB.1 probe still ran')
        need('USB input survey v2' not in boot_text, 'the survey did not run')
    return {'checked': True}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--hello', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    cfg = out / 'grub.cfg'
    cfg.write_text('serial --unit=0 --speed=115200\nterminal_input console serial\nterminal_output console serial\nset timeout=-1\nset default=0\n'
                   'echo VOS-REGRESSION-MENU\n' +
                   ''.join(f'menuentry "{k} {n}" {{\n    multiboot2 /boot/vos.bin {extra}\n    module2 /boot/hello.elf hello\n    boot\n}}\n' for k, n, extra in ENTRIES))
    iso = out / 'regression-bios.iso'
    tmp = out / 'producer-tmp'
    tmp.mkdir()
    with (out / 'producer.txt').open('x') as log:
        subprocess.run(['grub-mkrescue', '-d', '/usr/lib/grub/i386-pc', '-o', str(iso), '-graft-points',
                        f'/boot/vos.bin={args.kernel.resolve()}', f'/boot/hello.elf={args.hello.resolve()}', f'/boot/grub/grub.cfg={cfg}'],
                       stdout=log, stderr=log, check=True, env=dict(os.environ, TMPDIR=str(tmp)))
    results = []
    for index, (key, name, extra) in enumerate(ENTRIES):
        r = {'case': f'bios-{name}', 'cmdline': extra, 'passed': False}
        results.append(r)
        t0 = time.monotonic()
        try:
            r.update(check(key, boot(out, iso, index, key)))
            r['passed'] = True
        except Fail as e:
            r['failure'] = str(e)
        except Exception as e:  # noqa: BLE001 - recorded, not hidden
            r['failure'] = f'{type(e).__name__}: {e}'
        r['seconds'] = round(time.monotonic() - t0, 1)
        print(json.dumps(r), flush=True)
    summary = {'cases': len(results), 'passed': sum(r['passed'] for r in results), 'results': results,
               'inputs': {'kernel': sha(args.kernel), 'hello': sha(args.hello), 'iso': sha(iso)}, 'physical_hardware': False}
    (out / 'result.json').write_text(json.dumps(summary, indent=1) + '\n')
    print(f"{summary['passed']}/{summary['cases']} passed")
    sys.exit(0 if summary['passed'] == summary['cases'] else 1)


if __name__ == '__main__':
    main()

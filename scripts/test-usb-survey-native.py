#!/usr/bin/env python3
"""USB input bring-up, Increment 1: native QEMU checks of the read-only USB survey and of the unchanged default boot.

Boots the survey ISOs (menu entry 0: vos.usb-survey=hold) and the default ISOs under BIOS (SeaBIOS) and UEFI (OVMF) on
QEMU q35 with several USB topologies, and checks the guest's own account: every xHCI controller found on the bus tree,
its decoded registers, the connected devices, the PS/2 status line, the IOMMU line, the final "nothing was written"
line, and that the machine really halts afterwards. A screenshot of each hold case proves the text is on the display
(a physical machine has no serial port). The default ISOs must show no `usb:` line and finish their normal boot.
QEMU is an emulator: this does not show that a physical controller behaves the same, and it never will.
No case is retried; a failure is reported as it happened.
"""
import argparse
import json
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OVMF_CODE = '/usr/share/OVMF/OVMF_CODE_4M.fd'
OVMF_VARS = '/usr/share/OVMF/OVMF_VARS_4M.fd'

XHCI = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-mouse,bus=xhci.0']
# one controller on the root bus, one behind a PCIe root port, and an EHCI
TWO = ['-device', 'qemu-xhci,id=xa', '-device', 'usb-kbd,bus=xa.0', '-device', 'pcie-root-port,id=rp1,bus=pcie.0,chassis=1,slot=1',
       '-device', 'qemu-xhci,id=xb,bus=rp1', '-device', 'usb-mouse,bus=xb.0', '-device', 'usb-ehci,id=eh']
NONE = []


class Fail(Exception):
    pass


def need(cond, what):
    if not cond:
        raise Fail(what)


def qmp(sock_path, command, **args):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(15)
    s.connect(str(sock_path))
    f = s.makefile('rw')
    f.readline()
    for msg in ({'execute': 'qmp_capabilities'}, {'execute': command, 'arguments': args}):
        f.write(json.dumps(msg) + '\n')
        f.flush()
        reply = json.loads(f.readline())
    s.close()
    need('error' not in reply, f'{command}: {reply}')


def boot(out, name, fw, iso, devices, hold, shot=False):
    folder = out / name
    folder.mkdir()
    serial, sock = folder / 'serial.txt', folder / 'qmp'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', *devices, '-serial', f'file:{serial}', '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot']
    if fw == 'uefi':
        variables = folder / 'OVMF_VARS_4M.fd'
        shutil.copyfile(OVMF_VARS, variables)
        argv += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    marker = 'usb: survey complete' if hold else 'boot: checks passed'
    with (folder / 'qemu.txt').open('x') as log:
        proc = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 180
            text = ''
            while time.monotonic() < deadline:
                need(proc.poll() is None, 'QEMU exited early')
                text = serial.read_text(errors='replace') if serial.exists() else ''
                need('panic:' not in text, 'guest panic: ' + text[text.find('panic:'):][:200])
                if marker in text:
                    break
                time.sleep(1)
            need(marker in text, f'no "{marker}" within the deadline')
            if hold:
                before = serial.read_text(errors='replace')
                time.sleep(4)
                need(serial.read_text(errors='replace') == before, 'the guest kept printing after the survey: it did not halt')
                need(proc.poll() is None, 'QEMU exited after the survey')
                if shot:
                    qmp(sock, 'screendump', filename=str(folder / 'screen.png'), format='png')
                    time.sleep(1)
                    need((folder / 'screen.png').stat().st_size > 1000, 'the screenshot is empty')
            return text
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()


def lines(text):
    return [l for l in text.splitlines() if l.startswith('usb:')]


def common(text):
    u = '\n'.join(lines(text))
    need('usb: USB input survey: PCI configuration and xHCI registers are only read; nothing is written' in u, 'banner')
    need('usb: survey complete. Nothing was written. The machine is halted: power it off.' in u, 'closing line')
    need(re.search(r'usb: i8042 status 0x[0-9a-f]{2} ', u), 'PS/2 status line')
    need('ACPI DMAR and IVRS are NOT read, so an Intel or AMD IOMMU is UNKNOWN' in u, 'IOMMU limitation stated')
    need('INTEGRITY' not in text and 'ERROR' not in u and 'CHANGED' not in u and 'WARNING' not in u, 'no integrity error, change or warning')
    return u


def check_one(text, ports_connected):
    u = common(text)
    need(len(re.findall(r'usb: xhci ', u)) == 1, 'exactly one xHCI listed')
    need('0x1b36:0x000d' in u, 'the QEMU xHCI identity')
    need(re.search(r'hci 0x0100 caplen 0x40 slots 64 intr 16 ports 8 scratchpads 0 ac64 1', u), 'decoded capability registers')
    need(re.search(rf'ports read 8 connected {ports_connected}', u), f'{ports_connected} connected ports')
    need(len(re.findall(r'usb: SUMMARY xhci ', u)) == 1 and u.index('SUMMARY') > u.index('IOMMU:'), 'the summary comes last, after the details')
    need('protocols usb2.00 ports 5+4 usb3.00 ports 1+4' in u, 'supported-protocol capabilities')
    return {'xhci': 1}


def check_two(text):
    u = common(text)
    xs = re.findall(r'usb: xhci (\w\w):(\w\w)\.(\d) ', u)
    need(len(xs) == 2, f'two xHCI controllers listed: {xs}')
    need(any(b != '00' for b, _, _ in xs), 'one controller is behind a bridge (bus other than 0)')
    need(len(re.findall(r'usb: ehci ', u)) == 1, 'the EHCI is listed')
    need(len(re.findall(r'ports read 8 connected', u)) == 2, 'both controllers read')
    need(re.search(r'bridges [1-9]', u), 'a bridge was walked')
    need(sum(int(c) for c in re.findall(r'ports read \d+ connected (\d+)', u)) == 2, 'the keyboard and the mouse, one per controller')
    need(len(re.findall(r'usb: SUMMARY xhci ', u)) == 2, 'one summary per controller')
    return {'xhci': 2, 'behind_bridge': True}


def check_none(text):
    u = common(text)
    need('no xHCI controller found on the buses that were scanned' in u, 'absence stated')
    need(not re.search(r'usb: (xhci|ehci|uhci|ohci) ', u), 'no controller listed')
    return {'xhci': 0}


def check_default(text):
    need(not lines(text), 'the default boot printed a usb: line')
    need('proc[hello]: exited status 0, all frames reclaimed' in text and 'boot: checks passed' in text, 'normal boot completion')
    need('INTEGRITY' not in text and 'panic' not in text, 'no failure')
    return {'usb_lines': 0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel-dir', type=Path, required=True, help='directory holding vos.iso, vos-uefi.iso, vos-usb-survey.iso, vos-uefi-usb-survey.iso')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cases', help='comma-separated subset')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    k = args.kernel_dir.resolve()
    iso = {('bios', True): k / 'vos-usb-survey.iso', ('uefi', True): k / 'vos-uefi-usb-survey.iso',
           ('bios', False): k / 'vos.iso', ('uefi', False): k / 'vos-uefi.iso'}
    cases = []
    for fw in ('bios', 'uefi'):
        cases += [(f'{fw}-survey-one-xhci', fw, True, XHCI, lambda t: check_one(t, 2), True),
                  (f'{fw}-survey-two-xhci-bridge-ehci', fw, True, TWO, check_two, fw == 'bios'),
                  (f'{fw}-survey-no-usb', fw, True, NONE, check_none, False),
                  (f'{fw}-default-boot-with-xhci', fw, False, XHCI, check_default, False)]
    if args.cases:
        want = set(args.cases.split(','))
        cases = [c for c in cases if c[0] in want]
    results = []
    for name, fw, hold, devices, check, shot in cases:
        r = {'case': name, 'passed': False}
        results.append(r)
        t0 = time.monotonic()
        try:
            text = boot(out, name, fw, iso[(fw, hold)], devices, hold, shot)
            r.update(check(text))
            r['passed'] = True
        except Fail as e:
            r['failure'] = str(e)
        except Exception as e:  # noqa: BLE001 - recorded, not hidden
            r['failure'] = f'{type(e).__name__}: {e}'
        r['seconds'] = round(time.monotonic() - t0, 1)
        print(json.dumps(r), flush=True)
    summary = {'cases': len(results), 'passed': sum(r['passed'] for r in results), 'results': results,
               'qemu': subprocess.run(['qemu-system-x86_64', '--version'], capture_output=True, text=True).stdout.splitlines()[0]}
    (out / 'result.json').write_text(json.dumps(summary, indent=1) + '\n')
    print(f"{summary['passed']}/{summary['cases']} passed")
    sys.exit(0 if summary['passed'] == summary['cases'] else 1)


if __name__ == '__main__':
    main()

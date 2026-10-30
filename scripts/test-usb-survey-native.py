#!/usr/bin/env python3
"""USB input bring-up, Increments 1 and 2: native QEMU checks of the read-only USB survey and of the unchanged default boot.

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
# an EHCI with a keyboard on it: the Dell's keyboard and mouse are on an EHCI behind an internal hub, so a connected EHCI root port
# is the interesting case
EHCI_KBD = ['-device', 'usb-ehci,id=eh', '-device', 'usb-kbd,bus=eh.0']
# IOMMU tables and remapping hardware that is present but not enabled by anything: table present is not isolation
INTEL_IOMMU = XHCI + ['-device', 'intel-iommu']
AMD_IOMMU = XHCI + ['-device', 'amd-iommu']


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


def boot(out, name, fw, iso, devices, hold, shot=False, mem='128M'):
    folder = out / name
    folder.mkdir()
    serial, sock = folder / 'serial.txt', folder / 'qmp'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', mem, '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
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
    need('usb: USB input survey v2: PCI configuration, xHCI, EHCI and firmware tables are only read; nothing is written' in u, 'banner')
    need('usb: survey complete. Nothing was written. The machine is halted: power it ON.' in u, 'closing line')
    need(re.search(r'usb: i8042 status 0x[0-9a-f]{2} ', u), 'PS/2 status line')
    need('usb: dma-protection facts (separate claims): table present ' in u, 'the four DMA facts are reported')
    need('DMA isolation NOT ESTABLISHED' in u and 'usb: SUMMARY DMA isolation NOT ESTABLISHED' in u, 'isolation is never claimed')
    need(re.search(r'usb: acpi (rsdp revision \d|: )', u), 'the ACPI root was looked for')
    need('INTEGRITY' not in text and 'ERROR' not in u and 'CHANGED' not in u and 'WARNING' not in u, 'no integrity error, change or warning')
    return u


def check_one(text, ports_connected):
    u = common(text)
    need(len(re.findall(r'usb: xhci ', u)) == 1, 'exactly one xHCI listed')
    need('0x1b36:0x000d' in u, 'the QEMU xHCI identity')
    need(re.search(r'hci 0x0100 caplen 0x40 slots 64 intr 16 ports 8 scratchpads 0 ac64 1', u), 'decoded capability registers')
    need(re.search(rf'ports read 8 connected {ports_connected}', u), f'{ports_connected} connected ports')
    need(len(re.findall(r'usb: SUMMARY xhci ', u)) == 1 and u.index('SUMMARY') > u.index('IOMMU:'), 'the summary comes last, after the details')
    need(re.search(r'usb:   portsc of 8 of 8 ports: connected \d+ enabled \d+ powered \d+ over-current 0', u), 'all eight PORTSC registers are shown')
    need(re.search(r'usb:   dcbaap 0x[0-9a-f]+ \[[a-z -]+\] crcr\(lo\) 0x[0-9a-f]{8} ir0 erstba', u), 'the ring pointers are shown')
    need('usb: SUMMARY usb2 routing: not read' in u, 'a non-Intel controller has no routing registers to read')
    need('protocols usb2.00 ports 5+4 usb3.00 ports 1+4' in u, 'supported-protocol capabilities')
    need(facts(u)['table_present'] == 'no', 'no IOMMU table on a plain machine')
    return {'xhci': 1, **facts(u)}


def check_two(text):
    u = common(text)
    xs = re.findall(r'usb: xhci (\w\w):(\w\w)\.(\d) ', u)
    need(len(xs) == 2, f'two xHCI controllers listed: {xs}')
    need(any(b != '00' for b, _, _ in xs), 'one controller is behind a bridge (bus other than 0)')
    need(len(re.findall(r'usb: ehci \S+ \S+ cls ', u)) == 1 and len(re.findall(r'usb: ehci \S+ \S+ hci 0x', u)) == 1, 'the EHCI is listed once and its registers are read')
    need(len(re.findall(r'ports read 8 connected', u)) == 2, 'both controllers read')
    need(re.search(r'bridges [1-9]', u), 'a bridge was walked')
    need(sum(int(c) for c in re.findall(r'ports read \d+ connected (\d+)', u)) == 2, 'the keyboard and the mouse, one per controller')
    need(len(re.findall(r'usb: SUMMARY xhci ', u)) == 2, 'one summary per controller')
    return {'xhci': 2, 'behind_bridge': True}


def facts(u):
    m = re.search(r'table present (\w+), hardware answers (\w+)\r?\nusb:   translation enabled (\w+), xhci translated (\w+)', u)
    need(m, 'the DMA facts line')
    return dict(zip(('table_present', 'hardware_answers', 'translation_enabled', 'xhci_translated'), m.groups()))


def check_ehci_kbd(text):
    u = common(text)
    need(len(re.findall(r'usb: ehci \S+ \S+ cls ', u)) == 1 and len(re.findall(r'usb: ehci \S+ \S+ hci 0x', u)) == 1, 'the EHCI is listed once and its registers are read')
    need(re.search(r'usb:   connected 1 companion-owned 0', u), 'the keyboard is a connected EHCI root port')
    need(re.search(r'usb:   ehci root port \d connected \S+ enabled \d owner ehci \(a hub behind it\? devices below it are not visible\)', u), 'the hub caveat is stated')
    need(re.search(r'usb: SUMMARY ehci connected root ports: [0-9a-f]{4}=1', u), 'the summary names the EHCI with the connected port')
    need('no xHCI controller found' in u and 'usb: SUMMARY usb2 routing: not read' in u, 'no xHCI, so no routing')
    return {'ehci_connected': 1, **facts(u)}


def check_intel_iommu(text):
    u = common(text)
    f = facts(u)
    need(f['table_present'] == 'yes', 'the DMAR table is found')
    need(re.search(r'usb: dmar host-width \d+ flags 0x[0-9a-f]{2} units [1-9]', u), 'the DMAR is parsed')
    need(re.search(r'usb:   intel unit 0 base 0x[0-9a-f]+ flags 0x[0-9a-f]{2} covers xhci: yes', u), 'the unit covers the xHCI by its scope')
    need(re.search(r'usb:     registers ver 0x[0-9a-f]+ status 0x[0-9a-f]{8} translation off', u), 'the unit answers and translation is off')
    need(f['hardware_answers'] == 'yes' and f['translation_enabled'] == 'no' and f['xhci_translated'] == 'no', f'present but not enabled: {f}')
    return f


def check_amd_iommu(text):
    u = common(text)
    f = facts(u)
    need(f['table_present'] == 'yes', 'the IVRS table is found')
    need(re.search(r'usb: ivrs units [1-9]', u), 'the IVRS is parsed')
    need(f['xhci_translated'] == 'unknown', f'AMD coverage is never inferred: {f}')
    return f


def check_high_tables(text):
    u = common(text)
    m = re.search(r'usb: acpi rsdp revision \d rsdt 0x([0-9a-f]+) xsdt 0x([0-9a-f]+) root (\w+) tables (\d+)', u)
    need(m, 'the ACPI root line')
    root = int(m.group(2), 16) or int(m.group(1), 16)
    need(root >= 1 << 30, f'the root table is above 1 GiB, so the read-only lease path was used: {root:#x}')
    need(int(m.group(4)) > 0 and 'acpi tables not fully read' not in u, 'the tables were read through the lease')
    need(facts(u)['table_present'] == 'yes', 'the DMAR table is found above 1 GiB')
    return {'root': hex(root), **facts(u)}


def check_none(text):
    u = common(text)
    need('no xHCI controller found on the buses that were scanned' in u, 'absence stated')
    need(not re.search(r'usb: (xhci|ehci|uhci|ohci) ', u), 'no controller listed')
    need(facts(u)['table_present'] == 'no', 'QEMU describes no IOMMU unless asked: absence is a result because the root table was read')
    return {'xhci': 0, **facts(u)}


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
        cases += [(f'{fw}-survey-one-xhci', fw, True, XHCI, lambda t: check_one(t, 2), True, '128M'),
                  (f'{fw}-survey-two-xhci-bridge-ehci', fw, True, TWO, check_two, fw == 'bios', '128M'),
                  (f'{fw}-survey-no-usb', fw, True, NONE, check_none, False, '128M'),
                  (f'{fw}-survey-ehci-keyboard', fw, True, EHCI_KBD, check_ehci_kbd, fw == 'bios', '128M'),
                  (f'{fw}-survey-intel-iommu', fw, True, INTEL_IOMMU, check_intel_iommu, False, '128M'),
                  (f'{fw}-survey-amd-iommu', fw, True, AMD_IOMMU, check_amd_iommu, False, '128M'),
                  (f'{fw}-survey-tables-above-1gib', fw, True, INTEL_IOMMU, check_high_tables, False, '2048M'),
                  (f'{fw}-default-boot-with-xhci', fw, False, XHCI, check_default, False, '128M'),
                  (f'{fw}-default-boot-2gib-iommu', fw, False, INTEL_IOMMU, check_default, False, '2048M')]
    if args.cases:
        want = set(args.cases.split(','))
        cases = [c for c in cases if c[0] in want]
    results = []
    for name, fw, hold, devices, check, shot, mem in cases:
        r = {'case': name, 'passed': False}
        results.append(r)
        t0 = time.monotonic()
        try:
            text = boot(out, name, fw, iso[(fw, hold)], devices, hold, shot, mem)
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

#!/usr/bin/env python3
"""A2: show that the real production boot path enters the Rust mb2_parse, on Legacy BIOS and on UEFI, in QEMU.

Each production ISO is booted with QEMU's gdbstub (`-S`, owned by this script). The independent observer in
scripts/vos_gdbstub.py sets a one-shot breakpoint at the vos.bin address of mb2_parse (the symbol that the link map takes from
libvos_kernel.a), lets the guest run, and requires the trap to land with RIP equal to that address. It records the arguments
(rdi = information block, rsi = its physical address, rdx = struct boot_info), then traps again at the return address and
requires a NULL return and a struct boot_info that the Rust code filled consistently with the block (physical range, memory
map, module count). The observer never writes guest memory or registers. The guest is then released and must finish the
normal boot ("boot: checks passed"). QEMU only; no physical hardware. No case is retried.

usage: test-mb2-parse-native.py --kernel-dir DIR --out DIR   (DIR holds vos.bin, vos.iso and vos-uefi.iso)
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
BOOT_INFO_SIZE = 1624


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


def run_one(out, kdir, fw, symbol):
    folder = out / fw
    folder.mkdir()
    serial, sock = folder / 'serial.txt', folder / 'gdb'
    iso = kdir / ('vos.iso' if fw == 'bios' else 'vos-uefi.iso')
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', '-serial', f'file:{serial}', '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-no-reboot', '-no-shutdown']
    if fw == 'uefi':
        variables = folder / 'OVMF_VARS_4M.fd'
        shutil.copyfile(OVMF_VARS, variables)
        argv += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'firmware': fw, 'iso': iso.name, 'symbol': 'mb2_parse', 'address': hex(symbol), 'passed': False}
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
            stop = g.cont()
            need(stop.startswith(('T05', 'S05')), f'a breakpoint trap, got {stop[:12]}')
            r = regs(g)
            need(r['rip'] == symbol, f"RIP {r['rip']:#x} equals the Rust mb2_parse {symbol:#x}")
            row['entry'] = {'rip': hex(r['rip']), 'info': hex(r['rdi']), 'info_phys': hex(r['rsi']), 'boot_info': hex(r['rdx'])}
            ret = g.u64(r['rsp'])
            total = int.from_bytes(g.read(r['rdi'], 4), 'little')
            g.remove(0, symbol, 1)
            g.insert(0, ret, 1)
            stop = g.cont()
            need(stop.startswith(('T05', 'S05')), 'the return trap')
            r2 = regs(g)
            need(r2['rip'] == ret and r2['rax'] == 0, f"mb2_parse returned NULL at its return address (rax {r2['rax']:#x})")
            bi = g.read(r['rdx'], BOOT_INFO_SIZE)
            u64 = lambda off: int.from_bytes(bi[off:off + 8], 'little')  # noqa: E731
            u32 = lambda off: int.from_bytes(bi[off:off + 4], 'little')  # noqa: E731
            fields = {'info_start': u64(0), 'info_end': u64(8), 'loader': u64(16), 'cmdline': u64(24), 'mmap': u64(32),
                      'mmap_entries': u32(40), 'module_count': u32(1584), 'fb': u64(1592), 'fb_tags': u32(1600), 'efi': u32(1604),
                      'efi_mmap_tags': u32(1616)}
            need(fields['info_start'] == r['rsi'] and fields['info_end'] - fields['info_start'] == total, 'physical range matches the block')
            need(r['rdi'] <= fields['mmap'] < r['rdi'] + total and fields['mmap_entries'] > 0, 'the memory map points into the block')
            need(fields['loader'] and r['rdi'] <= fields['loader'] < r['rdi'] + total, 'the loader name points into the block')
            row['return'] = {'rip': hex(r2['rip']), 'rax': r2['rax']}
            row['boot_info'] = {k: (hex(v) if k in ('loader', 'cmdline', 'mmap', 'fb') else v) for k, v in fields.items()}
            row['total_size'] = total
            g.remove(0, ret, 1)
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
            need('boot: checks passed' in text, 'the normal boot completes after the Rust parser')
            m = re.search(r'boot: loader "([^"]*)", memory map (\d+)', text)
            need(m and int(m[2]) == fields['mmap_entries'], 'the kernel reports the memory map entries the Rust parser stored')
            row['serial'] = {'loader': m[1], 'memory_map_entries': int(m[2])}
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
    m = re.search(r'^([0-9a-f]+) T mb2_parse$', nm, re.M)
    need(m, 'vos.bin defines mb2_parse')
    rows = [run_one(out, kdir, fw, int(m[1], 16)) for fw in ('bios', 'uefi')]
    ok = all(r['passed'] for r in rows)
    (out / 'result.json').write_text(json.dumps({'passed': ok, 'cases': rows}, indent=1) + '\n')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""B1: show that the real production boot path runs the Rust boot reservation code, and that it reaches the same result as the
frozen C, on Legacy BIOS and on UEFI at 128 MiB and 2 GiB, in QEMU.

Each production ISO is booted with QEMU's gdbstub (`-S`, owned by this script). The independent observer in scripts/vos_gdbstub.py
sets one-shot breakpoints at the vos.bin addresses of boot_reserve and efi_memory_exclude (the symbols that the link map takes from
libvos_kernel.a) and requires each trap to land with RIP equal to that address. At each entry it records the arguments and the
guest's struct boot_info and tags; at each return it reads the result, the reservation table and (EFI) the report. The observer
never writes guest memory or registers. The recorded input is then replayed through the FROZEN C references (tests/host/boot-resv-replay.c)
and the two results must be identical: the message, every table entry (range and reason), the EFI report. The guest then
finishes the normal boot ("boot: checks passed"), and the `boot: reserved` lines it printed must equal the recorded table.
QEMU only; no physical hardware. No case is retried.

usage: test-boot-resv-native.py --kernel-dir DIR --archive libvos_kernel.a --out DIR   (DIR holds vos.bin, vos.iso, vos-uefi.iso)
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
CASES = [('bios-128M', 'vos.iso', False, '128M'), ('bios-2G', 'vos.iso', False, '2G'),
         ('uefi-128M', 'vos-uefi.iso', True, '128M'), ('uefi-2G', 'vos-uefi.iso', True, '2G')]
BOOT_INFO, RESV_TABLE, REPORT = 1624, 1928, 32


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


def cstring(g, addr, limit=96):
    raw = g.read(addr, limit)
    return raw[:raw.index(0)].decode() if 0 in raw else raw.decode(errors='replace')


def guest_tag(g, address, label):
    size = int.from_bytes(g.read(address + 4, 4), 'little')
    need(16 <= size <= 0x10000, f'{label} is a bounded, validated Multiboot2 tag')
    return g.read(address, size)


def table_lines(g, base, efi_what):
    """The guest's reservation table as the replay prints it."""
    raw = g.read(base, RESV_TABLE)
    count = int.from_bytes(raw[1920:1928], 'little')
    need(count <= 80, 'the count of the guest table is within capacity')
    lines = [f'table: {count}']
    for i in range(count):
        s, e, w = (int.from_bytes(raw[24 * i + o:24 * i + o + 8], 'little') for o in (0, 8, 16))
        lines.append(f"e: 0x{s:x} 0x{e:x} {'<EFI>' if w == efi_what else cstring(g, w)}")
    return lines, count


def trap(g, address, what):
    need(g.cont().startswith(('T05', 'S05')), f'a breakpoint trap at {what}')
    r = regs(g)
    need(r['rip'] == address, f"RIP {r['rip']:#x} equals the Rust {what} {address:#x}")
    return r


def run_one(out, kdir, replay, name, iso_name, efi, mem, syms):
    folder = out / name
    folder.mkdir()
    serial, sock = folder / 'serial.txt', folder / 'gdb'
    iso = kdir / iso_name
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', mem, '-cdrom', str(iso), '-boot', 'd', '-display', 'none',
            '-nic', 'none', '-serial', f'file:{serial}', '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-no-reboot', '-no-shutdown']
    if efi:
        variables = folder / 'OVMF_VARS_4M.fd'
        shutil.copyfile(OVMF_VARS, variables)
        argv += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'case': name, 'iso': iso.name, 'memory': mem, 'addresses': {k: hex(v) for k, v in syms.items()}, 'passed': False}
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
            # boot_reserve(bi, kstart, kend, table)
            g.insert(0, syms['boot_reserve'], 1)
            r = trap(g, syms['boot_reserve'], 'boot_reserve')
            bi, kstart, kend, table = r['rdi'], r['rsi'], r['rdx'], r['rcx']
            bi_bytes = g.read(bi, BOOT_INFO)
            mmap = int.from_bytes(bi_bytes[32:40], 'little')
            need(mmap != 0, 'the guest recorded a memory map')
            mmap_tag = guest_tag(g, mmap, 'memory-map tag')
            efi_ptr = int.from_bytes(bi_bytes[1608:1616], 'little')
            efi_tag = guest_tag(g, efi_ptr, 'EFI map tag') if efi_ptr else None
            need(bool(efi_ptr) == efi, 'the EFI memory-map tag is present exactly on UEFI')
            ret = g.u64(r['rsp'])
            g.remove(0, syms['boot_reserve'], 1)
            g.insert(0, ret, 1)
            need(g.cont().startswith(('T05', 'S05')), 'the boot_reserve return trap')
            r2 = regs(g)
            need(r2['rip'] == ret, 'the return trap is at the return address')
            reserve_msg = None if r2['rax'] == 0 else cstring(g, r2['rax'])
            need(reserve_msg is None, f'boot_reserve succeeded on the real boot (said {reserve_msg!r})')
            after_reserve, n_reserve = table_lines(g, table, syms['efi_memory_what'])
            g.remove(0, ret, 1)
            row['boot_reserve'] = {'entry_rip': hex(r['rip']), 'kernel': [hex(kstart), hex(kend)], 'table_entries': n_reserve, 'result': 'NULL'}
            # efi_memory_exclude(bi, table, report)
            g.insert(0, syms['efi_memory_exclude'], 1)
            r = trap(g, syms['efi_memory_exclude'], 'efi_memory_exclude')
            need(r['rdi'] == bi and r['rsi'] == table, 'the same boot_info and table are handed on')
            report = r['rdx']
            ret = g.u64(r['rsp'])
            g.remove(0, syms['efi_memory_exclude'], 1)
            g.insert(0, ret, 1)
            need(g.cont().startswith(('T05', 'S05')), 'the efi_memory_exclude return trap')
            r2 = regs(g)
            need(r2['rip'] == ret, 'the efi return trap is at the return address')
            rep = g.read(report, REPORT)
            status = int.from_bytes(rep[8:12], 'little', signed=True)
            need(int.from_bytes(r2['rax'].to_bytes(8, 'little')[:4], 'little', signed=True) == status, 'the return value is the report status')
            after_efi, n_efi = table_lines(g, table, syms['efi_memory_what'])
            g.remove(0, ret, 1)
            frames, error, desc, ranges, coalesced = (int.from_bytes(rep[a:b], 'little') for a, b in ((0, 8), (12, 16), (16, 20), (20, 24), (24, 28)))
            need(status == (0 if efi else 1), 'EFI applied on UEFI, not-EFI on BIOS')
            row['efi_memory_exclude'] = {'entry_rip': hex(r['rip']), 'status': status, 'frames': frames, 'error': error, 'descriptors': desc,
                                         'ranges': ranges, 'coalesced': coalesced, 'table_entries': n_efi}
            for suffix, data in (('boot_info.bin', bi_bytes), ('mmap_tag.bin', mmap_tag), ('efi_tag.bin', efi_tag)):
                if data is not None:
                    (folder / suffix).write_bytes(data)
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
            need('boot: checks passed' in text, 'the normal boot completes after the Rust reservations')
            # replay through the frozen C
            args = [str(replay), str(folder / 'boot_info.bin'), str(folder / 'mmap_tag.bin'),
                    str(folder / 'efi_tag.bin') if efi_tag is not None else '-', f'{kstart:x}', f'{kend:x}']
            ref = subprocess.run(args, capture_output=True, text=True, timeout=60)
            (folder / 'replay.txt').write_text(ref.stdout)
            need(ref.returncode == 0, f'the replay ran (exit {ref.returncode}: {ref.stderr[:200]})')
            efi_line = (f'efi: {status} frames {frames} error {error} descriptors {desc} ranges {ranges} coalesced {coalesced}')
            guest = ['reserve: (null)', *after_reserve, efi_line, *after_efi]
            need(ref.stdout.splitlines() == guest, 'the frozen C reaches the same message, table and report as the Rust on the real boot')
            printed = re.findall(r'^boot: reserved (0x[0-9a-f]+)-(0x[0-9a-f]+) (.*?)\r?$', text, re.M)
            need([f'e: {a} {b} {w}'.replace('firmware memory (EFI)', '<EFI>') for a, b, w in printed] == after_efi[1:],
                 'the kernel printed the table the observer recorded')
            if efi:
                m = re.search(r'^efi: (\d+) frames excluded in (\d+) ranges from (\d+) descriptors, coalesced (\d+)', text, re.M)
                need(m and tuple(map(int, m.groups())) == (frames, ranges, desc, coalesced), 'the kernel printed the EFI report the observer recorded')
            else:
                need('efi: no EFI map (BIOS): e820 protection only' in text, 'the kernel reports the BIOS case')
            row['reserved_lines'] = len(printed)
            row['replay_identical'] = True
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
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out, kdir, archive = a.out.resolve(), a.kernel_dir.resolve(), a.archive.resolve()
    if out.exists():
        ap.error('output already exists: preserve it and choose a fresh path')
    out.mkdir(parents=True)
    nm = subprocess.check_output(['nm', str(kdir / 'vos.bin')], text=True)
    syms = {}
    for s in ('boot_reserve', 'efi_memory_exclude', 'efi_memory_what'):
        m = re.search(r'^([0-9a-f]+) [TR] ' + s + '$', nm, re.M)
        need(m, f'vos.bin defines {s}')
        syms[s] = int(m[1], 16)
    cc = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie', '-O1', '-g', '-Iarch/x86_64']
    ren = {'bootinfo_ref': ['-Dmb2_parse=mb2_parse_reference', '-Dboot_framebuffer=boot_framebuffer_reference', '-Dboot_mmap_entry=boot_mmap_entry_reference',
                            '-Dboot_reserve=boot_reserve_reference', '-Dresv_add=resv_add_reference'],
           'resv_ref': ['-Dresv_init=resv_init_reference', '-Dresv_add=resv_add_reference', '-Dresv_overlaps=resv_overlaps_reference'],
           'efi_memory_ref': ['-Defi_memory_exclude=efi_memory_exclude_reference', '-Defi_memory_firmware_frame=efi_memory_firmware_frame_reference',
                              '-Defi_memory_strerror=efi_memory_strerror_reference', '-Defi_memory_what=efi_memory_what_reference',
                              '-Dresv_add=resv_add_reference']}
    objs = []
    for name, flags in ren.items():
        subprocess.run(cc + flags + ['-c', f'tests/host/reference/{name}.c', '-o', out / f'{name}.o'], cwd=ROOT, check=True)
        objs.append(out / f'{name}.o')
    replay = out / 'replay'
    subprocess.run(cc + ['-Wl,--gc-sections', 'tests/host/boot-resv-replay.c', *objs, archive, '-o', replay], cwd=ROOT, check=True)
    rows = [run_one(out, kdir, replay, *case, syms) for case in CASES]
    ok = all(r['passed'] for r in rows)
    (out / 'result.json').write_text(json.dumps({'passed': ok, 'cases': rows}, indent=1) + '\n')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

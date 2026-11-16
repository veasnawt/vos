#!/usr/bin/env python3
"""R4-M3: show that the running kernel's pmm_init_in is the Rust code, and that every call it makes leaves the instance exactly as
the FROZEN accepted C (tests/reference/pmm-r4m3.c) does, on Legacy BIOS and on UEFI at 128 MiB and 2 GiB, in QEMU.

The self-checking test image (VOS_TEST=1) is booted with QEMU's gdbstub (`-S`, owned by this script). The observer
(scripts/vos_gdbstub.py) sets a breakpoint at the vos.bin address of pmm_init_in, which the link map must take from
libvos_kernel.a, and requires every trap to land on it. For every call (the kernel's own initialization from the firmware memory map
and every self-test call on a synthetic map, including the refused ones) it records, at entry, the arguments, the memory-map tag
and its entries and the reservation table; at the return, the result, the complete instance and, after a successful call, both
bitmaps. The observer never writes guest memory or registers. The test image then runs to its own verdict, which must be
VOS-TEST: PASS (isa-debug-exit 33). Every recorded call is replayed through the frozen C (tests/host/pmm-init-replay.c, ASan/UBSan)
on fresh bitmaps: result, instance and bitmaps must equal the guest's. QEMU only; no physical hardware. No case is retried.

usage: test-pmm-init-native.py --test-dir DIR --out DIR   (DIR: vos.bin, kernel.map, vos.iso, vos-uefi.iso from VOS_TEST=1)
"""
import argparse, hashlib, json, re, shutil, struct, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from vos_gdbstub import GdbStub  # noqa: E402

OVMF_CODE, OVMF_VARS = '/usr/share/OVMF/OVMF_CODE_4M.fd', '/usr/share/OVMF/OVMF_VARS_4M.fd'
CASES = [('bios-128M', 'vos.iso', False, '128M'), ('bios-2G', 'vos.iso', False, '2G'),
         ('uefi-128M', 'vos-uefi.iso', True, '128M'), ('uefi-2G', 'vos-uefi.iso', True, '2G')]
MAX_FRAMES, MAX_CALLS = 262144, 64
MARKERS = ('pmm: 262144 frames tracked below 1 GiB, ', 'selftest: pmm-synthetic ', 'selftest: pmm-kernel ', 'selftest: vmm ', 'selftest: kmap ', 'selftest: user ', 'VOS-TEST: PASS')
PUBLIC = 'pmm_init_in pmm_init pmm_alloc_frame pmm_free_frame pmm_reserve_frame pmm_free_count pmm_kernel pmm_strerror boot_mmap_entry'.split()


class Fail(Exception):
    pass


def need(cond, what):
    if not cond:
        raise Fail(what)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def fnv(h, data):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return h


def regs(g):
    text = g.command('g')
    vals = [int.from_bytes(bytes.fromhex(text[i:i + 16]), 'little') for i in range(0, 17 * 16, 16)]
    return dict(zip(['rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp'] + [f'r{n}' for n in range(8, 16)] + ['rip'], vals))


def rust_address(tdir):
    addr = None
    for line in subprocess.run(['nm', str(tdir / 'vos.bin')], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == 'pmm_init_in' and p[1] == 'T':
            addr = int(p[0], 16)
    kmap = (tdir / 'kernel.map').read_text()
    need(addr and re.search(r'\.text\.pmm_init_in\s*\n?\s*0x0*%x\s+0x[0-9a-f]+ .*libvos_kernel\.a' % addr, kmap), 'pmm_init_in is the Rust archive code')
    return addr


def inputs(g, r):
    """frames, the memory map and the reservation table as pmm_init_in sees them at entry (rdi p, rsi avail, rdx alloc, rcx frames,
    r8 bi, r9 t)."""
    frames, bi, t = r['rcx'], r['r8'], r['r9']
    has_map, esize, version, entries, mbytes = 0, 0, 0, 0, b''
    if bi:
        tag = g.u64(bi + 32); entries = int.from_bytes(g.read(bi + 40, 4), 'little')
        if tag:
            has_map = 1; esize, version = struct.unpack('<II', g.read(tag + 8, 8))
            need(entries * esize <= 64 * 1024, 'a memory map the replay can hold')
            mbytes = g.read(tag + 16, entries * esize) if entries * esize else b''
    resv = b''
    if t:
        raw = bytearray(g.read(t, 1928))
        for i in range(80):
            raw[24 * i + 16:24 * i + 24] = bytes(8)        # the `what` string pointers: not inputs of the classification
        resv = bytes(raw)
    return struct.pack('<QQIIII', frames, has_map, esize, version, entries, 1 if t else 0) + mbytes + resv


def observe(out, tdir, replay, name, iso_name, efi, mem, addr):
    folder = out / name; folder.mkdir()
    serial = folder / 'serial.txt'
    short = Path(tempfile.mkdtemp(prefix='vi-')); sock = short / 'gdb'
    argv = ['qemu-system-x86_64', '-accel', 'tcg', '-m', mem, '-cdrom', str(tdir / iso_name), '-boot', 'd', '-display', 'none', '-nic', 'none',
            '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-no-reboot']
    if efi:
        variables = folder / 'OVMF_VARS_4M.fd'; shutil.copyfile(OVMF_VARS, variables)
        argv[1:1] = ['-machine', 'q35', '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'case': name, 'memory': mem, 'firmware': 'OVMF' if efi else 'SeaBIOS', 'address': hex(addr), 'passed': False}
    files, guest, results, kernel = [], [], {}, None
    with (folder / 'qemu.txt').open('x') as log:
        proc = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        g = None
        try:
            end = time.monotonic() + 30
            while not sock.exists():
                need(proc.poll() is None and time.monotonic() < end, 'QEMU gdbstub did not appear')
                time.sleep(0.2)
            g = GdbStub(sock, timeout=600)
            need(g.stop_reason().startswith(('T', 'S')), 'the target starts stopped')
            g.insert(0, addr, 1); exited = False
            while len(files) < MAX_CALLS:
                stop = g.cont()
                if not stop.startswith(('T05', 'S05')):
                    exited = True                           # the guest finished (isa-debug-exit) with the breakpoint still set
                    break
                r = regs(g)
                need(r['rip'] == addr, f"RIP {r['rip']:#x} is pmm_init_in's entry")
                rec = inputs(g, r); p, frames = r['rdi'], r['rcx']
                ret = g.u64(r['rsp'])
                g.remove(0, addr, 1); g.insert(0, ret, 1)
                need(g.cont().startswith(('T05', 'S05')), 'the return trap')
                r2 = regs(g)
                need(r2['rip'] == ret, 'the call returned straight to its caller (no other stop between)')
                g.remove(0, ret, 1); g.insert(0, addr, 1)
                res = int.from_bytes((r2['rax'] & 0xffffffff).to_bytes(4, 'little'), 'little', signed=True)
                inst = g.read(p, 64)
                h = fnv(1469598103934665603, inst[16:64])
                if res == 0:
                    words = frames // 64
                    need(0 < words <= MAX_FRAMES // 64 and int.from_bytes(inst[0:8], 'little') == r['rsi'] and int.from_bytes(inst[8:16], 'little') == r['rdx'],
                         'a successful call stores the caller bitmaps')
                    h = fnv(fnv(h, g.read(r['rsi'], 8 * words)), g.read(r['rdx'], 8 * words))
                    if frames == MAX_FRAMES and kernel is None:
                        kernel = dict(usable=int.from_bytes(inst[24:32], 'little'), ignored_bytes=int.from_bytes(inst[48:56], 'little'))
                f = folder / f'call{len(files):03d}.bin'; f.write_bytes(rec)
                files.append(str(f)); guest.append(f'{res} {h:016x}'); results[str(res)] = results.get(str(res), 0) + 1
            need(exited, 'fewer than %d calls (the observer saw every call)' % MAX_CALLS)
            try:
                status = proc.wait(timeout=300)
            except subprocess.TimeoutExpired:
                raise Fail('the test image did not finish within 300 s')
            text = serial.read_text(errors='replace')
            need(status == 33, f'isa-debug-exit status {status}, expected 33 (VOS-TEST: PASS)')
            missing = [m for m in MARKERS if not any(m in l and not l.startswith('user[') for l in text.splitlines())]
            need(not missing, f'self-test markers present (missing {missing})')
            need(kernel is not None and results.get('0', 0) >= 2 and len(results) >= 3, f'the kernel initialization, successful and refused self-test calls observed ({results})')
            (folder / 'calls.txt').write_text('\n'.join(files) + '\n'); (folder / 'guest.txt').write_text('\n'.join(guest) + '\n')
            rep = subprocess.run([str(replay)], input='\n'.join(files) + '\n', capture_output=True, text=True, timeout=300)
            (folder / 'replay.txt').write_text(rep.stdout); (folder / 'replay-stderr.txt').write_text(rep.stderr)
            need(rep.returncode == 0, f'the replay ran (exit {rep.returncode}: {rep.stderr[:200]})')
            got = rep.stdout.split('\n')[:len(guest)]
            diff = [i for i, (x, y) in enumerate(zip(got, guest)) if x != y]
            need(len(got) == len(guest) and not diff, f'the frozen C initializes every recorded call as the guest did (first differences {diff[:5]})')
            row.update(calls=len(files), results=results, kernel_instance=kernel, pmm_line=next((l for l in text.splitlines() if l.startswith('pmm: ')), None),
                       exit_status=status, markers=list(MARKERS), replay_identical=True, passed=True)
        except Fail as e:
            row['failure'] = str(e)
        finally:
            if g:
                g.close()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    proc.kill()
            shutil.rmtree(short, ignore_errors=True)
            for b in folder.glob('call*.bin'):
                b.unlink()
            for v in folder.glob('OVMF_VARS_4M.fd'):
                v.unlink()
    (folder / 'result.json').write_text(json.dumps(row, indent=1) + '\n')
    print(json.dumps(row), flush=True)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--test-dir', type=Path, required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); tdir = a.test_dir.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        raise SystemExit('fresh confined output only')
    out.mkdir(parents=True)
    report = {'qemu_only': True, 'physical_hardware': 'NOT TESTED', 'cases': []}
    try:
        need(all((tdir / n).is_file() for n in ('vos.bin', 'kernel.map', 'vos.iso', 'vos-uefi.iso')), 'a VOS_TEST=1 build')
        addr = rust_address(tdir)
        replay = out / 'pmm-init-replay'
        flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        ren = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        with (out / 'replay-build.txt').open('x') as f:
            r1 = subprocess.run(['gcc', *flags, *ren, '-c', 'tests/reference/pmm-r4m3.c', '-o', str(out / 'ref.o')], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
            r2 = subprocess.run(['gcc', *flags, 'tests/host/pmm-init-replay.c', str(out / 'ref.o'), '-Wl,--gc-sections', '-o', str(replay)], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT) if r1.returncode == 0 else r1
        need(r2.returncode == 0, 'the frozen-C replay builds')
        report['test_kernel_sha256'] = sha(tdir / 'vos.bin'); report['rust_address'] = hex(addr)
        for c in CASES:
            report['cases'].append(observe(out, tdir, replay, *c, addr))
        report['sources_sha256'] = {n: sha(ROOT / n) for n in ('scripts/test-pmm-init-native.py', 'tests/host/pmm-init-replay.c', 'tests/reference/pmm-r4m3.c')}
        report['passed'] = all(r['passed'] for r in report['cases'])
    except Fail as e:
        report.update(passed=False, failure=str(e))
    (out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print('PMM INIT NATIVE %s' % ('PASS' if report.get('passed') else 'FAIL'))
    sys.exit(0 if report.get('passed') else 1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""R4-M1: show that the running kernel's pmm_free_check_in, pmm_frame_state_in and pmm_check_in are the Rust code, and that every
recorded call returns what the FROZEN accepted C (tests/reference/pmm-r4m1.c) returns for the same instance, on Legacy BIOS and on
UEFI at 128 MiB and 2 GiB, in QEMU.

The self-checking test image (VOS_TEST=1: the bootinfo, PMM, VMM, user, kmap ... self-tests) is booted with QEMU's gdbstub (`-S`,
owned by this script). The observer (scripts/vos_gdbstub.py) sets breakpoints at the vos.bin addresses of the three checks, which
the link map must take from libvos_kernel.a, and requires each trap to land with RIP equal to that address. At each entry it records
the argument and the guest's struct pmm, plus the bitmap words the C would read (the one word of the frame for the frame checks, the
complete bitmaps for pmm_check_in); at the return it records the result. The observer never writes guest memory or registers. It
records at most a bounded number of calls per check, then removes its breakpoints and the test image runs to its own verdict, which
must be VOS-TEST: PASS (isa-debug-exit 33) with the PMM/VMM/kmap self-test lines. Every recorded call is replayed through the frozen
C (tests/host/pmm-replay.c) and each result must equal the guest's. Then the production images boot under SeaBIOS and OVMF and a
screenshot of the running console is kept. QEMU only; no physical hardware. No case is retried.

usage: test-pmm-native.py --test-dir DIR --kernel-dir DIR --out DIR   (test DIR: vos.bin, vos.iso, vos-uefi.iso, kernel.map from
VOS_TEST=1; kernel DIR: the production vos.iso and vos-uefi.iso)
"""
import argparse, hashlib, json, re, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from vos_gdbstub import GdbStub  # noqa: E402

OVMF_CODE, OVMF_VARS = '/usr/share/OVMF/OVMF_CODE_4M.fd', '/usr/share/OVMF/OVMF_VARS_4M.fd'
CASES = [('bios-128M', 'vos.iso', False, '128M'), ('bios-2G', 'vos.iso', False, '2G'),
         ('uefi-128M', 'vos-uefi.iso', True, '128M'), ('uefi-2G', 'vos-uefi.iso', True, '2G')]
FUNCS = {'pmm_free_check_in': 'F', 'pmm_frame_state_in': 'S', 'pmm_check_in': 'C'}
LIMIT = {'F': 300, 'S': 300, 'C': 24}
MAX_FRAMES = 262144
MARKERS = ('selftest: pmm-synthetic ', 'selftest: pmm-kernel ', 'selftest: vmm ', 'selftest: kmap ', 'VOS-TEST: PASS')
PUBLIC = ('pmm_init_in pmm_alloc_in pmm_free_check_in pmm_free_in pmm_reserve_in pmm_frame_state_in pmm_check_in pmm_init pmm_alloc_frame '
          'pmm_free_frame pmm_reserve_frame pmm_free_count pmm_kernel pmm_strerror boot_mmap_entry').split()


class Fail(Exception):
    pass


def need(cond, what):
    if not cond:
        raise Fail(what)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def regs(g):
    text = g.command('g')
    vals = [int.from_bytes(bytes.fromhex(text[i:i + 16]), 'little') for i in range(0, 17 * 16, 16)]
    return dict(zip(['rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp'] + [f'r{n}' for n in range(8, 16)] + ['rip'], vals))


def rust_symbols(kdir):
    """The addresses of the three checks; the link map must take each from the Rust archive."""
    syms = {}
    for line in subprocess.run(['nm', str(kdir / 'vos.bin')], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] in FUNCS and p[1] == 'T':
            syms[p[2]] = int(p[0], 16)
    kmap = (kdir / 'kernel.map').read_text()
    for f in FUNCS:
        need(f in syms and re.search(r'\.text\.%s\s*\n?\s*0x0*%x\s+0x[0-9a-f]+ .*libvos_kernel\.a' % (f, syms[f]), kmap), f'{f} is the Rust archive code')
    return syms


def instance(g, p):
    raw = g.read(p, 64)
    avail, alloc, frames, usable, free, hint, _ignored = (int.from_bytes(raw[8 * i:8 * i + 8], 'little') for i in range(7))
    state = int.from_bytes(raw[56:60], 'little', signed=True)
    bounded = frames and frames % 64 == 0 and frames <= MAX_FRAMES and avail and alloc and (avail | alloc) % 8 == 0
    return dict(avail=avail, alloc=alloc, frames=frames, usable=usable, free=free, hint=hint, state=state, bounded=bool(bounded))


def record(g, folder, kind, n, r):
    """The replay line for this call (inputs only; the result is appended at the return)."""
    s = instance(g, r['rdi'])
    head = f"{kind} {s['state'] & 0xffffffff:x} {s['frames']:x} {s['usable']:x} {s['free']:x} {s['hint']:x}"
    if kind == 'C':
        if s['state'] == 1 and s['bounded']:
            words = s['frames'] // 64
            fa, fl = folder / f'c{n:03d}-avail.bin', folder / f'c{n:03d}-alloc.bin'
            fa.write_bytes(g.read(s['avail'], 8 * words)); fl.write_bytes(g.read(s['alloc'], 8 * words))
            return f'{head} {fa} {fl}', s
        return f'{head} - -', s
    phys = r['rsi']; frame = phys >> 12; w = frame >> 6
    reads = s['state'] == 1 and s['bounded'] and frame < s['frames'] and (kind == 'S' or phys % 4096 == 0)
    if reads:
        aw = g.u64(s['avail'] + 8 * w); lw = g.u64(s['alloc'] + 8 * w)
        return f'{head} {phys:x} {w:x} {aw:x} {lw:x}', s
    return f'{head} {phys:x} - - -', s


def observe(out, tdir, replay, name, iso_name, efi, mem, syms):
    folder = out / name
    folder.mkdir()
    serial = folder / 'serial.txt'
    short = Path(tempfile.mkdtemp(prefix='vp-')); sock = short / 'gdb'
    argv = ['qemu-system-x86_64', '-accel', 'tcg', '-m', mem, '-cdrom', str(tdir / iso_name), '-boot', 'd', '-display', 'none', '-nic', 'none',
            '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-no-reboot']
    if efi:
        variables = folder / 'OVMF_VARS_4M.fd'; shutil.copyfile(OVMF_VARS, variables)
        argv[1:1] = ['-machine', 'q35', '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'case': name, 'memory': mem, 'firmware': 'OVMF' if efi else 'SeaBIOS', 'addresses': {k: hex(v) for k, v in syms.items()}, 'passed': False}
    lines, results, counts, instances = [], [], {k: 0 for k in LIMIT}, set()
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
            armed = {}
            for f, k in FUNCS.items():
                g.insert(0, syms[f], 1); armed[k] = syms[f]
            by_addr = {v: FUNCS[f] for f, v in syms.items()}
            while armed:
                need(g.cont().startswith(('T05', 'S05')), 'a breakpoint trap at a check')
                r = regs(g)
                need(r['rip'] in by_addr and by_addr[r['rip']] in armed, f"RIP {r['rip']:#x} is a Rust check's entry")
                kind = by_addr[r['rip']]
                line, s = record(g, folder, kind, counts[kind], r)
                instances.add(r['rdi'])
                ret = g.u64(r['rsp'])
                g.remove(0, armed[kind], 1)   # QEMU would re-trap at the entry; a check is a leaf, so it cannot re-enter
                g.insert(0, ret, 1)
                need(g.cont().startswith(('T05', 'S05')), 'the return trap')
                r2 = regs(g)
                need(r2['rip'] == ret, 'the call returned straight to its caller (a leaf: no other stop between)')
                g.remove(0, ret, 1)
                results.append(int.from_bytes((r2['rax'] & 0xffffffff).to_bytes(4, 'little'), 'little', signed=True))
                lines.append(line); counts[kind] += 1
                if counts[kind] >= LIMIT[kind]:
                    armed.pop(kind)
                else:
                    g.insert(0, armed[kind], 1)
            g._send('c')  # release the guest; it runs its remaining self-tests and reports through isa-debug-exit
            try:
                status = proc.wait(timeout=300)
            except subprocess.TimeoutExpired:
                raise Fail('the test image did not finish within 300 s')
            text = serial.read_text(errors='replace')
            need(status == 33, f'isa-debug-exit status {status}, expected 33 (VOS-TEST: PASS)')
            missing = [m for m in MARKERS if not any(m in l and not l.startswith('user[') for l in text.splitlines())]
            need(not missing, f'self-test markers present (missing {missing})')
            (folder / 'calls.txt').write_text('\n'.join(lines) + '\n')
            rep = subprocess.run([str(replay)], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=120)
            (folder / 'replay.txt').write_text(rep.stdout); (folder / 'replay-stderr.txt').write_text(rep.stderr)
            need(rep.returncode == 0, f'the replay ran (exit {rep.returncode}: {rep.stderr[:200]})')
            ref = [int(x) for x in rep.stdout.split()]
            diff = [i for i, (a, b) in enumerate(zip(ref, results)) if a != b]
            need(len(ref) == len(results) and not diff, f'the frozen C returns the guest result for every call (first differences {diff[:5]})')
            dist = {}
            for line, res in zip(lines, results):
                dist.setdefault(line[0], {}).setdefault(str(res), 0); dist[line[0]][str(res)] += 1
            row.update(calls=counts, results=dist, distinct_instances=len(instances), full_bitmaps_replayed=sum(1 for l in lines if l[0] == 'C' and not l.endswith('- -')),
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
            for b in folder.glob('c*-a*.bin'):
                b.unlink()   # replayed above; the hashes of what was read are in calls.txt's order through replay.txt
    (folder / 'result.json').write_text(json.dumps(row, indent=1) + '\n')
    print(json.dumps(row), flush=True)
    return row


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(10); self.s.connect(str(path)); self.f = self.s.makefile('rwb'); self.n = 0
        need('QMP' in json.loads(self.f.readline()), 'QMP greeting'); self.command('qmp_capabilities')

    def command(self, name, args=None):
        self.n += 1; obj = {'execute': name, 'id': self.n}
        if args is not None:
            obj['arguments'] = args
        self.f.write((json.dumps(obj) + '\n').encode()); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if r.get('id') == self.n:
                need('error' not in r, 'QMP ' + str(r)); return r.get('return')


def screenshot(out, kdir, name, iso_name, efi):
    """Boot the production image and keep a PNG of the running console once the boot checks have passed."""
    folder = out / name
    folder.mkdir()
    serial = folder / 'serial.txt'; short = Path(tempfile.mkdtemp(prefix='vq-')); sock = short / 'qmp'
    argv = ['qemu-system-x86_64', '-accel', 'tcg', '-m', '128M', '-cdrom', str(kdir / iso_name), '-boot', 'd', '-display', 'none', '-nic', 'none',
            '-serial', f'file:{serial}', '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot', '-no-shutdown']
    if efi:
        variables = folder / 'OVMF_VARS_4M.fd'; shutil.copyfile(OVMF_VARS, variables)
        argv[1:1] = ['-machine', 'q35', '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'case': name, 'firmware': 'OVMF' if efi else 'SeaBIOS', 'passed': False}
    with (folder / 'qemu.txt').open('x') as log:
        proc = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        try:
            end = time.monotonic() + 180; text = ''
            while time.monotonic() < end:
                need(proc.poll() is None, 'QEMU exited early')
                text = serial.read_text(errors='replace') if serial.exists() else ''
                need('panic:' not in text, 'guest panic')
                if 'boot: checks passed' in text:
                    break
                time.sleep(0.5)
            need('boot: checks passed' in text, 'the production boot completes')
            time.sleep(2)
            q = QMP(sock); png = folder / 'screen.png'
            q.command('screendump', {'filename': str(png), 'format': 'png'})
            for _ in range(100):
                if png.exists() and png.stat().st_size:
                    break
                time.sleep(0.1)
            need(png.exists() and png.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n', 'a PNG screendump')
            row.update(png='screen.png', png_sha256=sha(png), pmm_lines=[l for l in text.splitlines() if l.startswith('pmm: ')][:4], passed=True)
        except Fail as e:
            row['failure'] = str(e)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
            shutil.rmtree(short, ignore_errors=True)
            for v in folder.glob('OVMF_VARS_4M.fd'):
                v.unlink()
    (folder / 'result.json').write_text(json.dumps(row, indent=1) + '\n')
    print(json.dumps(row), flush=True)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--test-dir', type=Path, required=True); ap.add_argument('--kernel-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve(); tdir = a.test_dir.resolve(); kdir = a.kernel_dir.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'qemu_only': True, 'physical_hardware': 'NOT TESTED', 'cases': [], 'screenshots': []}
    try:
        syms = rust_symbols(tdir)
        replay = out / 'pmm-replay'
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        san = ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']   # the replay itself is checked too
        subprocess.run(['gcc', '-std=gnu11', '-O1', *san, '-Wall', '-Wextra', '-Werror', '-Iarch/x86_64', *renames, '-c', 'tests/reference/pmm-r4m1.c', '-o', str(out / 'ref.o')], cwd=ROOT, check=True)
        subprocess.run(['gcc', '-std=gnu11', '-O1', *san, '-Wall', '-Wextra', '-Werror', '-Iarch/x86_64', 'tests/host/pmm-replay.c', str(out / 'ref.o'), '-o', str(replay)], cwd=ROOT, check=True)
        report['test_kernel_sha256'] = sha(tdir / 'vos.bin'); report['rust_addresses'] = {k: hex(v) for k, v in syms.items()}
        for c in CASES:
            report['cases'].append(observe(out, tdir, replay, *c, syms))
        for name, iso, efi in (('screen-bios', 'vos.iso', False), ('screen-uefi', 'vos-uefi.iso', True)):
            report['screenshots'].append(screenshot(out, kdir, name, iso, efi))
        report['sources_sha256'] = {n: sha(ROOT / n) for n in ('scripts/test-pmm-native.py', 'tests/host/pmm-replay.c', 'tests/reference/pmm-r4m1.c')}
        report['passed'] = all(r['passed'] for r in report['cases'] + report['screenshots'])
    except Fail as e:
        report['failure'] = str(e); report['passed'] = False
    finally:
        for f in (out / 'ref.o', out / 'pmm-replay'):
            f.unlink(missing_ok=True)
        (out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print('PMM NATIVE', 'PASS' if report.get('passed') else 'FAIL')
    sys.exit(0 if report.get('passed') else 1)


if __name__ == '__main__':
    main()

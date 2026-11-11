#!/usr/bin/env python3
"""R4-M2: show that the running kernel's pmm_alloc_in, pmm_free_in and pmm_reserve_in are the Rust code, and that sampled calls
leave the PMM instance exactly as the FROZEN accepted C (tests/reference/pmm-r4m2.c) does, on Legacy BIOS and on UEFI at 128 MiB and
2 GiB, in QEMU.

The self-checking test image (VOS_TEST=1) is booted with QEMU's gdbstub (`-S`, owned by this script). The observer
(scripts/vos_gdbstub.py) sets breakpoints at the vos.bin addresses of the three transitions, which the link map must take from
libvos_kernel.a, and requires every trap to land on one of them. For sampled calls it records, at entry, the guest's struct pmm and
both bitmaps (PMM_WORDS(frames) words each) and the argument; at the return, the result, the output word (alloc) and the complete
post-state. The observer never writes guest memory or registers. After a bounded number of calls it removes its breakpoints and the
test image runs to its own verdict, which must be VOS-TEST: PASS (isa-debug-exit 33). Every recorded call is replayed through the
frozen C (tests/host/pmm-transition-replay.c, ASan/UBSan) from the recorded pre-state: result, output word and post-state must equal
the guest's. QEMU only; no physical hardware. No case is retried.

usage: test-pmm-transition-native.py --test-dir DIR --rustc RUSTC --out DIR   (DIR: vos.bin, kernel.map, vos.iso, vos-uefi.iso from VOS_TEST=1)
"""
import argparse, hashlib, json, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from vos_gdbstub import GdbStub  # noqa: E402

OVMF_CODE, OVMF_VARS = '/usr/share/OVMF/OVMF_CODE_4M.fd', '/usr/share/OVMF/OVMF_VARS_4M.fd'
CASES = [('bios-128M', 'vos.iso', False, '128M'), ('bios-2G', 'vos.iso', False, '2G'),
         ('uefi-128M', 'vos-uefi.iso', True, '128M'), ('uefi-2G', 'vos-uefi.iso', True, '2G')]
FUNCS = {'pmm_alloc_in': 'A', 'pmm_free_in': 'F', 'pmm_reserve_in': 'R'}
RECORD = {'A': 16, 'F': 16, 'R': 16}          # sampled calls per kind and boot (any instance)
SMALL = 24                                     # plus, per kind, calls on instances of at most 16,384 frames (the self-tests' synthetic
                                               # instance, whose invalid frees and exhaustion exercise the failure paths)
EVERY = 37                                     # after the first four, every 37th call of a kind is recorded
HIT_BUDGET = 6000                              # breakpoint hits per boot before the observer lets the guest run free
MAX_FRAMES = 262144
MARKERS = ('selftest: pmm-synthetic ', 'selftest: pmm-kernel ', 'selftest: vmm ', 'selftest: kmap ', 'selftest: user ', 'VOS-TEST: PASS')
PUBLIC = ('pmm_init_in pmm_alloc_in pmm_free_in pmm_reserve_in pmm_init pmm_alloc_frame pmm_free_frame pmm_reserve_frame pmm_free_count '
          'pmm_kernel pmm_strerror boot_mmap_entry').split()


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


def rust_symbols(tdir):
    syms = {}
    for line in subprocess.run(['nm', str(tdir / 'vos.bin')], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] in FUNCS and p[1] == 'T':
            syms[p[2]] = int(p[0], 16)
    kmap = (tdir / 'kernel.map').read_text()
    for f in FUNCS:
        need(f in syms and re.search(r'\.text\.%s\s*\n?\s*0x0*%x\s+0x[0-9a-f]+ .*libvos_kernel\.a' % (f, syms[f]), kmap), f'{f} is the Rust archive code')
    return syms


def state(g, p):
    """The instance's 64 bytes and, when its bitmaps are readable within the documented storage, both bitmaps."""
    raw = g.read(p, 64)
    avail, alloc, frames = (int.from_bytes(raw[8 * i:8 * i + 8], 'little') for i in range(3))
    words = frames // 64
    if avail and alloc and avail % 8 == 0 and alloc % 8 == 0 and 0 < words <= MAX_FRAMES // 64:
        return raw, g.read(avail, 8 * words), g.read(alloc, 8 * words)
    return raw, b'', b''


def observe(out, tdir, replay, name, iso_name, efi, mem, syms):
    folder = out / name; folder.mkdir()
    serial = folder / 'serial.txt'
    short = Path(tempfile.mkdtemp(prefix='vt-')); sock = short / 'gdb'
    argv = ['qemu-system-x86_64', '-accel', 'tcg', '-m', mem, '-cdrom', str(tdir / iso_name), '-boot', 'd', '-display', 'none', '-nic', 'none',
            '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-no-reboot']
    if efi:
        variables = folder / 'OVMF_VARS_4M.fd'; shutil.copyfile(OVMF_VARS, variables)
        argv[1:1] = ['-machine', 'q35', '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=1) + '\n')
    row = {'case': name, 'memory': mem, 'firmware': 'OVMF' if efi else 'SeaBIOS', 'addresses': {k: hex(v) for k, v in syms.items()}, 'passed': False}
    lines, guest, hits, recorded, small = [], [], {k: 0 for k in RECORD}, {k: 0 for k in RECORD}, {k: 0 for k in RECORD}
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
            by_addr = {v: FUNCS[f] for f, v in syms.items()}
            for a in by_addr: g.insert(0, a, 1)
            total = 0
            while total < HIT_BUDGET:
                need(g.cont().startswith(('T05', 'S05')), 'a breakpoint trap at a transition')
                r = regs(g)
                need(r['rip'] in by_addr, f"RIP {r['rip']:#x} is a Rust transition's entry")
                kind = by_addr[r['rip']]; hits[kind] += 1; total += 1
                sampled = recorded[kind] < RECORD[kind] and (hits[kind] <= 4 or hits[kind] % EVERY == 0)
                frames = g.u64(r['rdi'] + 16)
                synthetic = not sampled and small[kind] < SMALL and 0 < frames <= 16384
                record = sampled or synthetic
                pre = state(g, r['rdi']) if record else None
                ret = g.u64(r['rsp'])
                g.remove(0, r['rip'], 1); g.insert(0, ret, 1)
                need(g.cont().startswith(('T05', 'S05')), 'the return trap')
                r2 = regs(g)
                need(r2['rip'] == ret, 'the call returned straight to its caller (no other stop between)')
                g.remove(0, ret, 1); g.insert(0, r['rip'], 1)
                if record:
                    post = state(g, r['rdi'])
                    res = int.from_bytes((r2['rax'] & 0xffffffff).to_bytes(4, 'little'), 'little', signed=True)
                    outw = g.u64(r['rsi']) if kind == 'A' else 0
                    pre_file = folder / f'{kind}{len(lines):03d}-pre.bin'
                    pre_file.write_bytes(pre[0] + pre[1] + pre[2])
                    h = fnv(fnv(fnv(1469598103934665603, post[0][16:64]), post[1]), post[2])
                    lines.append(f'{kind} {pre_file}' + ('' if kind == 'A' else f' {r["rsi"]:x}'))
                    guest.append(f'{res} {outw:x} {h:016x}')
                    if sampled: recorded[kind] += 1
                    else: small[kind] += 1
                if all(recorded[k] >= RECORD[k] and small[k] >= SMALL for k in ('A', 'F')):
                    break
            for a in by_addr: g.remove(0, a, 1)
            g._send('c')
            try:
                status = proc.wait(timeout=300)
            except subprocess.TimeoutExpired:
                raise Fail('the test image did not finish within 300 s')
            text = serial.read_text(errors='replace')
            need(status == 33, f'isa-debug-exit status {status}, expected 33 (VOS-TEST: PASS)')
            missing = [m for m in MARKERS if not any(m in l and not l.startswith('user[') for l in text.splitlines())]
            need(not missing, f'self-test markers present (missing {missing})')
            need(recorded['A'] >= 8 and recorded['F'] >= 8, f'enough transitions recorded {recorded}')
            (folder / 'calls.txt').write_text('\n'.join(lines) + '\n'); (folder / 'guest.txt').write_text('\n'.join(guest) + '\n')
            rep = subprocess.run([str(replay)], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=120)
            (folder / 'replay.txt').write_text(rep.stdout); (folder / 'replay-stderr.txt').write_text(rep.stderr)
            need(rep.returncode == 0, f'the replay ran (exit {rep.returncode}: {rep.stderr[:200]})')
            got = rep.stdout.split('\n')[:len(guest)]
            diff = [i for i, (x, y) in enumerate(zip(got, guest)) if x != y]
            need(len(got) == len(guest) and not diff, f'the frozen C leaves every recorded instance as the guest did (first differences {diff[:5]})')
            row.update(hits=hits, recorded=recorded, recorded_small_instances=small, results={k: sorted(set(l.split()[0] for l, c in zip(guest, lines) if c[0] == k)) for k in recorded},
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
            for b in folder.glob('*-pre.bin'):
                b.unlink()
            for v in folder.glob('OVMF_VARS_4M.fd'):
                v.unlink()
    (folder / 'result.json').write_text(json.dumps(row, indent=1) + '\n')
    print(json.dumps(row), flush=True)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--test-dir', type=Path, required=True)
    ap.add_argument('--rustc', required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve(); tdir = a.test_dir.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'qemu_only': True, 'physical_hardware': 'NOT TESTED', 'cases': []}
    root, lib = out / 'root.rs', out / 'libpmm-host.a'
    try:
        syms = rust_symbols(tdir)
        san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        renames = ['-D%s=ref_%s' % (s, s) for s in PUBLIC]
        root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / 'arch/x86_64/rust/pmm.rs')) + '] mod pmm;\n'
                        'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n'
                        '#[unsafe(no_mangle)] pub extern "C" fn rust_eh_personality() {}\n')
        subprocess.run([a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Coverflow-checks=yes', '-Cdebug-assertions=yes', '--crate-type=staticlib',
                        '-Cpanic=abort', str(root), '-o', str(lib)], cwd=ROOT, check=True)   # the accepted R4-M1 check, which the frozen pmm_free_in calls
        subprocess.run(['gcc', *san, *warn, *renames, '-c', 'tests/reference/pmm-r4m2.c', '-o', str(out / 'ref.o')], cwd=ROOT, check=True)
        replay = out / 'pmm-transition-replay'
        subprocess.run(['gcc', *san, *warn, 'tests/host/pmm-transition-replay.c', str(out / 'ref.o'), str(lib), '-Wl,--gc-sections', '-fno-pie', '-no-pie',
                        '-o', str(replay)], cwd=ROOT, check=True)
        report['test_kernel_sha256'] = sha(tdir / 'vos.bin'); report['rust_addresses'] = {k: hex(v) for k, v in syms.items()}
        for c in CASES:
            report['cases'].append(observe(out, tdir, replay, *c, syms))
        report['sources_sha256'] = {n: sha(ROOT / n) for n in ('scripts/test-pmm-transition-native.py', 'tests/host/pmm-transition-replay.c', 'tests/reference/pmm-r4m2.c')}
        report['passed'] = all(r['passed'] for r in report['cases'])
    except Fail as e:
        report['failure'] = str(e); report['passed'] = False
    finally:
        for f in (out / 'ref.o', out / 'pmm-transition-replay', root, lib):
            f.unlink(missing_ok=True)
        (out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print('PMM TRANSITION NATIVE', 'PASS' if report.get('passed') else 'FAIL')
    sys.exit(0 if report.get('passed') else 1)


if __name__ == '__main__':
    main()

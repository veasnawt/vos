#!/usr/bin/env python3
"""R3-N3 Rust virtio-net validation: the frozen accepted driver (afe33b8) and the production driver (C access sequence,
Rust decisions) run against one PCI/DMA device model and must write byte-identical transcripts of every decision and
every driver outcome, in a release build linked with the actual kernel archive and in an ASan/UBSan build with
overflow-checked Rust. Also: the accepted virtio-net host suite, named mutation controls that must change the
transcript, entry frames, panic freedom, the memory-symbol set, Rust independence from device/DMA symbols, symbol
ownership and a paired benchmark. Fresh confined outputs only."""
import argparse, csv, hashlib, io, json, os, re, shutil, statistics, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
RECORDS = 'TRANSCRIPT 216823 records'
ENTRIES = ['vos_rust_vnet_features_ok', 'vos_rust_vnet_queue_size_ok', 'vos_rust_vnet_queues_ok', 'vos_rust_vnet_used_count_ok', 'vos_rust_vnet_used_entry',
           'vos_rust_vnet_header_ok', 'vos_rust_vnet_notify_ok', 'vos_rust_vnet_send_length_ok', 'vos_rust_vnet_tx_wire',
           'vos_rust_vnet_capacity_ok', 'vos_rust_vnet_rx_outputs_overlap']
DRIVER_PUBLIC = ['vnet_init', 'vnet_send', 'vnet_receive', 'vnet_stop', 'vnet_info']
MEMORY_SYMBOLS = {'memcpy', 'memset', 'memcmp'}
DEVICE_SYMBOLS = re.compile(r'pci_|pmm_|vmm_|phys_to_virt|thread_|irq_|vblk_|vpci_|serial_')
# Equivalent mutants excluded after analysis: loosening the used-count bound (RX 8 -> 9, TX 1 -> 2) cannot change any
# outcome, because IDs must be distinct and below the bound, so any longer run repeats an ID and is refused (pigeonhole).
# Dropping the receive `capacity != 0` test is equivalent too: buffer_ok() refuses a zero length first, with the same EINVAL.
MUTATIONS = [  # (name, exact text in kernel/rust/net/vnet.rs, replacement); the transcript must change for each
 ('feat-version', 'pub const FEATURES: u64 = (1 << 32) | (1 << 5);', 'pub const FEATURES: u64 = 1 << 5;'),
 ('feat-mac', 'pub const FEATURES: u64 = (1 << 32) | (1 << 5);', 'pub const FEATURES: u64 = 1 << 32;'),
 ('feat-iommu', 'offered & FEATURES == FEATURES && offered & ACCESS_PLATFORM == 0', 'offered & FEATURES == FEATURES && offered & (ACCESS_PLATFORM << 1) == 0'),
 ('qsize-min', 'size >= QUEUE && size <= 32768', 'size > QUEUE && size <= 32768'),
 ('qsize-max', 'size >= QUEUE && size <= 32768', 'size >= QUEUE && size <= 65536'),
 ('qsize-pow2', 'size & size.wrapping_sub(1) == 0', 'size & 1 == 0'),
 ('queues', '    count >= 2', '    count >= 1'),
 ('count-rx', 'count <= if tx { 1 } else { QUEUE }', 'count <= if tx { 1 } else { QUEUE - 1 }'),
 ('count-tx', 'count <= if tx { 1 } else { QUEUE }', 'count <= if tx { 0 } else { QUEUE }'),
 ('id-limit', '    if id >= limit {', '    if id > limit {'),
 ('tx-id-limit', 'let limit = if tx { 1 } else { QUEUE };', 'let limit = if tx { 2 } else { QUEUE };'),
 ('duplicate', '    if seen & bit != 0 {', '    if seen & bit == u32::MAX {'),
 ('tx-length', 'let ok = if tx { len == 0 }', 'let ok = if tx { len <= 1 }'),
 ('rx-length-min', 'len >= HEADER + 14 && len <= HEADER + FRAME_MAX', 'len >= HEADER + 13 && len <= HEADER + FRAME_MAX'),
 ('rx-length-max', 'len >= HEADER + 14 && len <= HEADER + FRAME_MAX', 'len >= HEADER + 14 && len < HEADER + FRAME_MAX'),
 ('mask', '    seen | bit\n', '    seen\n'),
 ('header-flags', '    flags == 0 && gso == 0', '    flags < 2 && gso == 0'),
 ('header-gso', '    flags == 0 && gso == 0', '    flags == 0 && gso < 2'),
 ('notify-range', '!(offset > length as u64 ||', '!(offset > length as u64 + 1 ||'),
 ('notify-room', '(length as u64).wrapping_sub(offset) < 2)', '(length as u64).wrapping_sub(offset) < 1)'),
 ('send-min', '!(length < 14 || length > FRAME_MAX)', '!(length < 13 || length > FRAME_MAX)'),
 ('send-max', '!(length < 14 || length > FRAME_MAX)', '!(length < 14 || length > FRAME_MAX + 1)'),
 ('tx-wire', 'if length < 60 { 60 } else { length }', 'if length < 59 { 60 } else { length }'),
 ('capacity-max', '    capacity != 0 && capacity <= FRAME_MAX', '    capacity != 0 && capacity <= FRAME_MAX + 1'),
 ('overlap-length', 'frame < length.wrapping_add(4)', 'frame < length.wrapping_add(3)'),
 ('overlap-capacity', 'length < frame.wrapping_add(capacity as u64)', 'length < frame.wrapping_add(capacity as u64).wrapping_sub(1)'),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def frame(asm, name):
    m = re.search(r'^[0-9a-f]+ <' + re.escape(name) + r'>:\n(.*?)(?=\n\n|\Z)', asm, re.M | re.S); need(m, 'function ' + name); b = m[1]
    subs = [int(x, 16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp', b)]; adds = [int(x, 16) for x in re.findall(r'\tadd\s+\$0x([0-9a-f]+),%rsp', b)]
    return 8 * len(re.findall(r'\tpush\s', b)) + sum(x for x in subs if x < 1 << 63) + sum((1 << 64) - x for x in adds if x >= 1 << 63)
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True); ap.add_argument('--rustc', required=True); ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); archive = a.archive.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build') and archive.is_file(), 'fresh confined output'); out.mkdir(parents=True)
    report = {'status': 'RUNNING', 'archive_sha256': sha(archive), 'commands': [], 'seed': '52334e3356534e45'}
    def run(cmd, name, expect=0, timeout=1500, check=True):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = p.returncode
        if check: need(p.returncode == expect, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        ref = json.loads((ROOT / 'tests/reference/net-r3n3.json').read_text())
        for n, v in ref['sources_sha256'].items(): need(sha(ROOT / 'tests/reference' / n) == v, 'frozen accepted source ' + n)
        for n, v in ref['headers_sha256'].items():
            need(sha(ROOT / 'tests/reference/net-headers' / n) == v, 'frozen header ' + n); need(sha(ROOT / 'arch/x86_64' / n.replace('-', '_')) == v, 'production header unchanged ' + n)
        report['oracle'] = {'baseline': ref['baseline'], 'driver_sha256': ref['sources_sha256']['virtio-net.c']}
        report['gcc'] = subprocess.run(['gcc', '--version'], capture_output=True, text=True).stdout.splitlines()[0]
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections', '-fno-pie', '-no-pie']
        # virtio_blk.c only supplies the shared PCI capability parser and window check; built once, unsanitized, so that
        # --gc-sections can drop its unrelated block-device paths in every link.
        vblk = out / 'vblk.o'; run(['gcc', '-O2', *warn, '-c', 'arch/x86_64/virtio_blk.c', '-o', vblk], 'vblk-build.txt')
        common = [a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Cdebuginfo=0', '-Coverflow-checks=yes', '-Ccodegen-units=1']
        def hostlib(net_rs, lib, log):
            root = lib.with_suffix('.rs')
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(net_rs)) + '] mod net;\nunsafe extern "C" {fn abort()->!;}\n'
                            '#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
            run(common + ['--crate-type=staticlib', '-Cpanic=abort', root, '-o', lib], log)
        checked = out / 'libchecked.a'; hostlib(ROOT / 'arch/x86_64/rust/net.rs', checked, 'checked-rust-build.txt')
        DRV = {'ref': '-DVNET_DRIVER="../reference/virtio-net.c"', 'cand': '-DVNET_DRIVER="../../arch/x86_64/virtio_net.c"'}
        modes = {'release': (['-O2'], archive), 'sanitizer': (['-O2', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'], checked)}
        transcripts = {}
        for mode, (flags, lib) in modes.items():
            for which in ('ref', 'cand'):
                b = out / ('%s-%s' % (mode, which)); extra = [lib] if which == 'cand' else []
                run(['gcc', *flags, *warn, DRV[which], 'tests/host/vnet-differential.c', vblk, *extra, *link, '-o', b], b.name + '-build.txt')
                t = out / (b.name + '.transcript'); r = run([b, t], b.name + '.txt'); need(RECORDS in r, 'exact transcript coverage ' + b.name)
                transcripts[(mode, which)] = t.read_bytes()
            need(transcripts[(mode, 'ref')] == transcripts[(mode, 'cand')], 'frozen and production transcripts identical (%s)' % mode)
        need(transcripts[('release', 'ref')] == transcripts[('sanitizer', 'ref')], 'transcript independent of build mode')
        report['transcript'] = {'records': int(RECORDS.split()[1]), 'bytes': len(transcripts[('release', 'ref')]), 'sha256': hashlib.sha256(transcripts[('release', 'ref')]).hexdigest(),
                                'identical': ['release (production archive)', 'ASan/UBSan (overflow-checked Rust)']}
        # the accepted virtio-net host suite (real driver, PCI/DMA model, reset quarantine) against the Rust decisions
        run(['gcc', '-O2', *warn, 'tests/host/virtio-net.c', vblk, checked, *link, '-o', out / 'host-net'], 'host-net-build.txt')
        report['host_suite'] = run([out / 'host-net'], 'host-net.txt').strip(); need('251 checks PASS' in report['host_suite'], 'virtio-net host suite 251 checks')
        # paired benchmark with kernel-equivalent string codegen: the kernel is -ffreestanding -fno-builtin, so memcpy is always
        # an out-of-line call; host fortification would otherwise inline bounded copies in one build only.
        kflags = ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', '-fno-builtin']
        bench = {}
        for which in ('ref', 'cand'):
            b = out / ('bench-' + which); run(['gcc', *kflags, *warn, DRV[which], 'tests/host/vnet-differential.c', vblk, archive, *link, '-o', b], b.name + '-build.txt'); bench[which] = b
        report['cpu'] = min(os.sched_getaffinity(0)); samples = {'ref': [], 'cand': []}
        for i in range(10):
            for which in (('ref', 'cand') if i % 2 == 0 else ('cand', 'ref')):
                rows = list(csv.DictReader(io.StringIO(run(['taskset', '-c', str(report['cpu']), bench[which], '--bench'], 'bench-%s-%d.csv' % (which, i)))))
                samples[which].append({k: statistics.median(float(x[k]) for x in rows) for k in ('rx_ns', 'tx_ns')})
        report['performance'] = {}
        for k, label in (('rx_ns', 'receive_validate_copy_republish'), ('tx_ns', 'send_and_completion')):
            c = statistics.median(s[k] for s in samples['ref']); r = statistics.median(s[k] for s in samples['cand'])
            report['performance'][label] = {'C_ns': c, 'Rust_ns': r, 'percent': 100 * (r / c - 1)}
        report['regressions_over_5_percent'] = sorted(k for k, v in report['performance'].items() if v['percent'] > 5)
        # named mutation controls: each mutant core must change the release transcript
        source = (ROOT / 'kernel/rust/net/vnet.rs').read_text(); need(len({n for n, _, _ in MUTATIONS}) == len(MUTATIONS) >= 25, 'at least 25 uniquely named mutation controls')
        for name, old, new in MUTATIONS: need(source.count(old) == 1, 'unique named mutation ' + name)
        report['mutations'] = {}
        for name, old, new in MUTATIONS:
            d = out / 'mutants' / name; (d / 'arch/x86_64/rust').mkdir(parents=True); (d / 'kernel/rust/net').mkdir(parents=True)
            shutil.copy(ROOT / 'arch/x86_64/rust/net.rs', d / 'arch/x86_64/rust/net.rs')
            for m in sorted(q.stem for q in (ROOT / 'kernel/rust/net').glob('*.rs') if q.stem != 'vnet'): shutil.copy(ROOT / 'kernel/rust/net' / (m + '.rs'), d / 'kernel/rust/net' / (m + '.rs'))  # every other module net.rs includes
            (d / 'kernel/rust/net/vnet.rs').write_text(source.replace(old, new))
            lib = d / 'libmutant.a'; hostlib(d / 'arch/x86_64/rust/net.rs', lib, 'mutant-%s-build.txt' % name)
            run(['gcc', '-O2', *warn, DRV['cand'], 'tests/host/vnet-differential.c', vblk, lib, *link, '-o', d / 'diff'], 'mutant-%s-link.txt' % name)
            t = d / 'transcript'; run([d / 'diff', t], 'mutant-%s.txt' % name, check=False, timeout=600)
            exit_code = report['commands'][-1]['exit']; changed = not t.exists() or t.read_bytes() != transcripts[('release', 'ref')]
            need(changed, 'mutation control not detected: ' + name)
            report['mutations'][name] = {'detected': True, 'exit': exit_code, 'mutant_sha256': sha(d / 'kernel/rust/net/vnet.rs')}
            shutil.rmtree(d)
        # frames: Rust entries, and the C driver entries before (frozen) and after (production), kernel flags, this host's gcc
        kernel = ['-O2', '-fno-lto', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-mno-red-zone', '-mgeneral-regs-only', '-mcmodel=kernel']
        cobj = {}
        for which, src in (('ref', 'tests/reference/virtio-net.c'), ('cand', 'arch/x86_64/virtio_net.c')):
            o = out / ('kernel-%s-virtio_net.o' % which); run(['gcc', *kernel, *warn, '-c', src, '-o', o], o.name + '.txt'); cobj[which] = run(['objdump', '-d', '--no-show-raw-insn', o], o.name + '.dis')
        asm = run(['objdump', '-dr', '--no-show-raw-insn', archive], 'production-disassembly.txt')
        report['frames'] = {'rust': {n: frame(asm, n) for n in ENTRIES}, 'c_driver': {n: {'C_frozen': frame(cobj['ref'], n), 'C_production': frame(cobj['cand'], n)} for n in DRIVER_PUBLIC}}
        report['frame_growth'] = {n: v for n, v in report['frames']['c_driver'].items() if v['C_production'] > v['C_frozen']}
        funcs = dict(re.findall(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n\n|\Z)', asm, re.M | re.S))
        ours = {n: b for n, b in funcs.items() if n.startswith('vos_rust_vnet_') or '3net4vnet' in n}
        panics = sorted(n for n, b in ours.items() if re.search(r'R_X86_64_\w+\s+\S*(?:panic|unwrap_failed|expect_failed|bounds_check|slice_index|len_mismatch)', b))
        report['panic_free'] = {'functions': len(ours), 'with_panic_paths': panics}; need(len(ours) >= len(ENTRIES) and not panics, 'R3-N3 Rust code has no panic path')
        refs = sorted({t for b in ours.values() for t in re.findall(r'R_X86_64_\w+\s+([A-Za-z_][\w.$]*?)(?:[-+]0x[0-9a-f]+)?$', b, re.M) if not t.startswith(('.', '_R'))})
        report['rust_external_symbols'] = refs
        need(set(refs) <= MEMORY_SYMBOLS and not any(DEVICE_SYMBOLS.match(t) for t in refs), 'Rust validation references no device, DMA or mapping symbol: %s' % refs)
        symbols = run(['nm', '--defined-only', archive], 'archive-owners.txt')
        for n in ENTRIES: need(sum(x.endswith(' T ' + n) for x in symbols.splitlines()) == 1, 'one Rust owner ' + n)
        o = out / 'kernel-cand-virtio_net.o'; csyms = run(['nm', o], 'C-symbols-virtio_net.txt')
        for n in DRIVER_PUBLIC: need(' T ' + n in csyms, 'C keeps the driver ABI ' + n)
        for n in ENTRIES: need(' U ' + n in csyms, 'C driver uses the Rust decision ' + n)
        report['source_sha256'] = {n: sha(ROOT / n) for n in ['kernel/rust/net/vnet.rs', 'arch/x86_64/rust/net.rs', 'arch/x86_64/virtio_net.c', 'arch/x86_64/rust_kernel.h',
            'tests/host/vnet-differential.c', 'tests/reference/virtio-net.c', 'tests/reference/net-headers/virtio_net.h', 'tests/reference/net-r3n3.json', 'scripts/test-vnet-rust.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'mutations', 'source_sha256')}, indent=2)[:5000])
    print('mutation controls detected:', len(report.get('mutations', {})))
if __name__ == '__main__': main()

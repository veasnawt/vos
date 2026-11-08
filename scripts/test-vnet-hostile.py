#!/usr/bin/env python3
"""R3-N3-S: virtio-net RX used-ring re-read fix. A deterministic hostile device (tests/host/vnet-hostile.c) rewrites the head
used entry right after the driver's validation read. The corrected driver must read the entry once and use the validated
values in every case; the frozen accepted f3683869 driver must still demonstrate the defect (negative control); deliberate
reintroductions of the second read must be detected; and for an honest device the corrected driver must write a transcript
byte-identical to the frozen driver's (tests/host/vnet-differential.c). ASan/UBSan throughout. A paired benchmark of the frozen
and corrected drivers, linked with the actual kernel archive, reports any regression over 5%. Fresh confined output only."""
import argparse, csv, hashlib, io, json, os, re, shutil, statistics, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CASES = 440
RECORDS = 'TRANSCRIPT 216823 records'
MUTATIONS = [  # (name, exact text in arch/x86_64/virtio_net.c, replacement): the hostile regression must reject each
 ('reread-both', '    uint32_t id=(uint32_t)head&(VNET_QUEUE-1),n=((uint32_t)head>>3)-VNET_HEADER;', '    uint32_t id=q->entries[q->consumed%VNET_QUEUE].id,n=q->entries[q->consumed%VNET_QUEUE].len-VNET_HEADER;'),
 ('reread-id', '    uint32_t id=(uint32_t)head&(VNET_QUEUE-1),n=((uint32_t)head>>3)-VNET_HEADER;', '    uint32_t id=q->entries[q->consumed%VNET_QUEUE].id,n=((uint32_t)head>>3)-VNET_HEADER;'),
 ('reread-len', '    uint32_t id=(uint32_t)head&(VNET_QUEUE-1),n=((uint32_t)head>>3)-VNET_HEADER;', '    uint32_t id=(uint32_t)head&(VNET_QUEUE-1),n=q->entries[q->consumed%VNET_QUEUE].len-VNET_HEADER;'),
 ('head-from-last-entry', '        if (!n) head=len<<3|id;', '        head=len<<3|id;'),
 ('head-len-dropped', '    return (int32_t)head;', '    return (int32_t)(head&(VNET_QUEUE-1));'),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def parse(text):
    rows = {}
    for l in text.splitlines():
        if l.startswith('case '): rows[l.split()[1]] = l
    return rows
def field(line, k):
    m = re.search(r'\b' + k + r'=(\S+)', line); return m.group(1) if m else None
def safe_violations(rows):
    """Return the list of property violations for a driver that must be safe against the hostile device."""
    bad = []
    for name, l in rows.items():
        if 'VIOLATION' in l: bad.append(name + ': memory violation'); continue
        if name.startswith('tx-'): continue
        if field(l, 'id_reads') not in ('0', '1') or field(l, 'len_reads') not in ('0', '1'): bad.append(name + ': head entry read more than once')
        m = re.match(r'c\d+-id(\d)-', name)
        if m and not (field(l, 'rc') == '0' and field(l, 'n') == '60' and field(l, 'first') == '%02x' % (0x40 + int(m.group(1)))):
            bad.append(name + ': validated packet not delivered')
        if name.startswith('multi3-') and not (field(l, 'rc') == '0' and field(l, 'n') == '88'): bad.append(name + ': multi-completion head not used')
        if name.startswith('cap8-') and field(l, 'rc') != '5': bad.append(name + ': short capacity not EMSGSIZE')
        if name.startswith('cap60-') and not (field(l, 'rc') == '0' and field(l, 'n') == '60'): bad.append(name + ': validated length not used')
        if name in ('invalid-initial-id9', 'invalid-initial-len', 'duplicate-ids', 'bad-header') and field(l, 'rc') != '8': bad.append(name + ': invalid input not refused')
        if name == 'zero-count' and not (field(l, 'rc') == '4' and field(l, 'id_reads') == '0'): bad.append(name + ': zero count used head values')
    return bad
def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('--archive', type=Path, required=True); ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True); a = ap.parse_args(); out = a.out.resolve(); archive = a.archive.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build') and archive.is_file(), 'fresh confined output'); out.mkdir(parents=True)
    report = {'status': 'RUNNING', 'archive_sha256': sha(archive), 'commands': []}
    def run(cmd, name, expect=0, timeout=1500, check=True, env=None):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, env=env)
        row['exit'] = p.returncode
        if check: need(p.returncode == expect, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        ref = json.loads((ROOT / 'tests/reference/net-r3n3s.json').read_text())
        for n, v in ref['sources_sha256'].items(): need(sha(ROOT / 'tests/reference' / n) == v, 'frozen accepted source ' + n)
        warn = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Iarch/x86_64']
        link = ['-Wl,--gc-sections']; san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        vblk = out / 'vblk.o'; run(['gcc', '-O2', *warn, '-c', 'arch/x86_64/virtio_blk.c', '-o', vblk], 'vblk-build.txt')
        root = out / 'net-rust-root.rs'; lib = out / 'libnet.a'
        root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / 'arch/x86_64/rust/net.rs')) + '] mod net;\nunsafe extern "C" {fn abort()->!;}\n'
                        '#[panic_handler] fn p(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
        run([a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Coverflow-checks=yes', '--crate-type=staticlib', '-Cpanic=abort', root, '-o', lib], 'rust-build.txt')
        env = dict(os.environ, ASAN_OPTIONS='handle_segv=0:detect_leaks=0')   # the test's own SIGSEGV/SIGTRAP handlers drive the hostile device
        def hostile(driver, tag, src_lines=None):
            b = out / ('hostile-' + tag); run(['gcc', *san, *warn, '-DVNET_DRIVER="%s"' % driver, 'tests/host/vnet-hostile.c', vblk, lib, *link, '-o', b], b.name + '-build.txt')
            text = run([b], b.name + '.txt', env=env, timeout=900); need('HOSTILE DONE' in text, 'hostile run complete ' + tag)
            rows = parse(text); need(len(rows) == CASES, 'exact hostile case count %s %d' % (tag, len(rows))); return rows, text
        fixed, _ = hostile('../../arch/x86_64/virtio_net.c', 'fixed')
        bad = safe_violations(fixed); report['fixed'] = {'cases': len(fixed), 'violations': bad}
        need(not bad, 'corrected driver unsafe: %s' % bad[:5])
        frozen, ftext = hostile('../reference/virtio-net-r3n3.c', 'frozen')
        mem = sorted(n for n, l in frozen.items() if 'VIOLATION' in l); reread = sorted(n for n, l in frozen.items() if field(l, 'id_reads') == '2')
        wrong = sorted(n for n, l in frozen.items() if 'VIOLATION' not in l and l != fixed[n] and (field(l, 'rc'), field(l, 'n'), field(l, 'out')) != (field(fixed[n], 'rc'), field(fixed[n], 'n'), field(fixed[n], 'out')))
        ubsan = len(re.findall(r'runtime error: index \d+ out of bounds for type .uint64_t \[11\]', (out / 'hostile-frozen.txt').read_text()))
        report['negative_control'] = {'memory_violations': len(mem), 'second_reads': len(reread), 'wrong_results': len(wrong), 'ubsan_pool_out_of_bounds': ubsan,
                                      'examples': {'violation': mem[:3], 'wrong': wrong[:3]}}
        need(len(mem) >= 100 and ubsan >= 50 and len(wrong) >= 100 and len(reread) >= 200, 'frozen driver still demonstrates the defect: %s' % report['negative_control'])
        need(all(fixed[n] == frozen[n].replace('id_reads=2 len_reads=2', 'id_reads=1 len_reads=1') or n.startswith(('c', 'multi', 'cap'))
                 for n in fixed if n.startswith(('tx-', 'zero-', 'invalid-', 'duplicate-'))), 'non-hostile paths unchanged')
        # deliberate reintroductions of the second read and other breakages of the snapshot must be rejected
        source = (ROOT / 'arch/x86_64/virtio_net.c').read_text(); report['mutations'] = {}
        for name, old, new in MUTATIONS:
            need(source.count(old) == 1, 'unique mutation ' + name)
            d = out / 'mutants' / name; d.mkdir(parents=True); (d / 'virtio_net.c').write_text(source.replace(old, new))
            rows, _ = hostile(str(d / 'virtio_net.c'), 'mutant-' + name)
            v = safe_violations(rows); need(v, 'mutation not detected: ' + name)
            report['mutations'][name] = {'detected': True, 'violations': len(v), 'first': v[0]}
            shutil.rmtree(d)
        # honest device: the corrected driver's transcript equals the frozen driver's (release and sanitizer)
        report['transcripts'] = {}
        for mode, flags in (('release', ['-O2']), ('sanitizer', san)):
            t = {}
            for tag, drv in (('frozen', '../reference/virtio-net-r3n3.c'), ('fixed', '../../arch/x86_64/virtio_net.c')):
                b = out / ('diff-%s-%s' % (mode, tag)); run(['gcc', *flags, *warn, '-DVNET_DRIVER="%s"' % drv, 'tests/host/vnet-differential.c', vblk, lib, *link, '-o', b], b.name + '-build.txt')
                f = out / (b.name + '.transcript'); need(RECORDS in run([b, f], b.name + '.txt'), 'transcript coverage ' + b.name); t[tag] = f.read_bytes()
            need(t['frozen'] == t['fixed'], 'honest-device transcripts identical (%s)' % mode); report['transcripts'][mode] = {'identical': True, 'sha256': hashlib.sha256(t['fixed']).hexdigest()}
        # paired benchmark, frozen vs corrected driver, with kernel-equivalent string codegen (-fno-builtin: memcpy stays a call)
        kflags = ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', '-fno-builtin']; bench = {}
        for tag, drv in (('frozen', '../reference/virtio-net-r3n3.c'), ('fixed', '../../arch/x86_64/virtio_net.c')):
            b = out / ('bench-' + tag); run(['gcc', *kflags, *warn, '-DVNET_DRIVER="%s"' % drv, 'tests/host/vnet-differential.c', vblk, archive, *link, '-o', b], b.name + '-build.txt'); bench[tag] = b
        report['cpu'] = min(os.sched_getaffinity(0)); samples = {'frozen': [], 'fixed': []}
        for i in range(10):
            for tag in (('frozen', 'fixed') if i % 2 == 0 else ('fixed', 'frozen')):
                rows = list(csv.DictReader(io.StringIO(run(['taskset', '-c', str(report['cpu']), bench[tag], '--bench'], 'bench-%s-%d.csv' % (tag, i)))))
                samples[tag].append({k: statistics.median(float(x[k]) for x in rows) for k in ('rx_ns', 'tx_ns')})
        report['performance'] = {}
        for k, label in (('rx_ns', 'receive_validate_copy_republish'), ('tx_ns', 'send_and_completion')):
            f = statistics.median(s[k] for s in samples['frozen']); x = statistics.median(s[k] for s in samples['fixed'])
            report['performance'][label] = {'frozen_ns': f, 'fixed_ns': x, 'percent': 100 * (x / f - 1)}
        report['regressions_over_5_percent'] = sorted(k for k, v in report['performance'].items() if v['percent'] > 5)
        run(['gcc', '-O2', *warn, 'tests/host/virtio-net.c', vblk, lib, *link, '-o', out / 'host-net'], 'host-net-build.txt')
        report['host_suite'] = run([out / 'host-net'], 'host-net.txt').strip(); need('251 checks PASS' in report['host_suite'], 'virtio-net host suite')
        report['source_sha256'] = {n: sha(ROOT / n) for n in ['arch/x86_64/virtio_net.c', 'tests/host/vnet-hostile.c', 'tests/host/vnet-differential.c', 'tests/reference/virtio-net-r3n3.c',
                                                              'tests/reference/net-r3n3s.json', 'scripts/test-vnet-hostile.py']}
        report['status'] = 'PASS'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'source_sha256')}, indent=2)[:4000])
if __name__ == '__main__': main()

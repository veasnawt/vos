#!/usr/bin/env python3
"""Bounded USB.1 review measurements. No hardware, downloads or source changes.

Use an existing verified kernel archive and a fresh output directory. Only the
child process is pinned; host/VM power policy and Windows scheduling stay intact.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args(); out = a.out.resolve(); archive = a.archive.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build') or not archive.is_file():
        p.error('existing archive and fresh output below checkout build/ required')
    out.mkdir(parents=True)
    result = {'passed': False, 'classification': 'isolated operation and complete mocked-probe benchmarks',
              'hardware_or_boot_latency': False, 'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'commands': [], 'trials': [], 'affinity_CPU': sorted(os.sched_getaffinity(0))[0],
              'warmup': '3 x 50000 per implementation/operation/corpus',
              'samples': '3 independent processes x 9 alternating-order pairs x 300000 calls'}

    def run(argv, name, pin=False):
        result['commands'].append(list(map(str,argv)))
        def affinity(): os.sched_setaffinity(0, {result['affinity_CPU']})
        with (out/name).open('xb') as stream:
            r = subprocess.run(list(map(str,argv)),cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,
                               timeout=90,preexec_fn=affinity if pin else None)
        if r.returncode: raise RuntimeError(f'{name}: exit {r.returncode}')
        return (out/name).read_text()

    try:
        result['gcc'] = run(['gcc','--version'],'gcc.txt').splitlines()[0]
        result['ld'] = run(['ld','--version'],'ld.txt').splitlines()[0]
        run(['lscpu'],'CPU.txt')
        reference = ROOT/'tests/reference/xhci-usb1.c'
        original = subprocess.check_output(['git','show','95eb0e0c944277a4ed9f3ad0e8671e25d2d9f6cd:arch/x86_64/xhci.c'],cwd=ROOT)
        # USB.2-A changes no production C; use its locally retained accepted
        # parent rather than requiring acquisition of the later merge object.
        if reference.read_bytes() != original: raise RuntimeError('reference changed')
        flags = ['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-pie','-Iarch/x86_64']
        run(flags+['-ffreestanding','-fno-builtin','-mcmodel=kernel','-mno-red-zone','-mgeneral-regs-only',
                   '-fcf-protection=none','-fno-stack-protector','-fno-asynchronous-unwind-tables',
                   '-Dxhci_probe=reference_xhci_probe','-Dxhci_decode_caps=reference_xhci_decode_caps',
                   '-Dxhci_class_matches=reference_xhci_class_matches','-c',reference,'-o',out/'reference.o'],'C-build.txt')
        run(flags+['-no-pie','-Wl,--gc-sections','tests/bench/xhci-discovery.c','arch/x86_64/xhci.c',
                   out/'reference.o',archive,'-o',out/'bench'],'bench-build.txt')
        run(['objdump','-dr',out/'reference.o'],'C-assembly.txt')
        run(['objdump','-d','--disassemble=xhci_probe',out/'bench'],'Rust-FFI-assembly.txt')
        for n in range(3):
            rows=[json.loads(line) for line in run([out/'bench'],f'trial-{n+1}.jsonl',True).splitlines()]
            if len(rows)!=108:raise RuntimeError('unexpected sample count')
            pairs={}
            for row in rows:
                key=(row['operation'],row['corpus'],row['pair'])
                pairs.setdefault(key,{})[row['implementation']]=row
            for pair in pairs.values():
                if pair['C']['checksum']!=pair['Rust']['checksum']:raise RuntimeError('unequal semantic work')
            result['trials'].append(rows)
        summary={}
        for operation in ['class','decode','probe']:
            for corpus in ['positive','mixed']:
                ratios=[];values={'C':[],'Rust':[]}
                for rows in result['trials']:
                    selected=[r for r in rows if r['operation']==operation and r['corpus']==corpus]
                    for n in range(9):
                        pair={r['implementation']:r for r in selected if r['pair']==n}
                        ratios.append(pair['Rust']['cpu_ns']/pair['C']['cpu_ns'])
                        for name,row in pair.items():values[name].append(row['cpu_ns']/row['iterations'])
                summary[operation+'-'+corpus]={name:{'median_cpu_ns':statistics.median(v),'min':min(v),'max':max(v)} for name,v in values.items()}
                summary[operation+'-'+corpus]['paired_CPU_ratio']={'median':statistics.median(ratios),'min':min(ratios),'max':max(ratios),'pairs':len(ratios)}
        result['summary']=summary;result['passed']=True
    except BaseException as e:
        result['error']=str(e);raise
    finally:
        result['source_sha256']={str(q.relative_to(ROOT)):hashlib.sha256(q.read_bytes()).hexdigest() for q in
                                  [ROOT/'tests/bench/xhci-discovery.c',ROOT/'tests/reference/xhci-usb1.c',Path(__file__)]}
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2))


if __name__=='__main__':main()

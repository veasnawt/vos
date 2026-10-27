#!/usr/bin/env python3
"""Isolated R1 hosted comparison. No production writes, downloads or QEMU timing."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
BASE='0291b1ab8f7d7d491bd2f276efa59a46b638d400'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive',type=Path,required=True)
    p.add_argument('--user',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--cpu',type=int)
    p.add_argument('--samples',type=int,default=15)
    p.add_argument('--scalar-ops',type=int,default=1<<22)
    a=p.parse_args();out=a.out.resolve();archive=a.archive.resolve();user=a.user.resolve()
    if out.exists():p.error('output exists: retain it and choose a fresh directory')
    allowed=sorted(os.sched_getaffinity(0));cpu=a.cpu if a.cpu is not None else allowed[-1]
    if cpu not in allowed:p.error('requested CPU is outside process affinity')
    if not 3<=a.samples<=31 or not 256<=a.scalar_ops<=1<<26:p.error('sample/operation bound')
    out.mkdir(parents=True)
    commands=[]

    def run(args,name):
        commands.append(list(map(str,args)))
        with (out/name).open('xb') as f:
            x=subprocess.run(list(map(str,args)),cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,
                             timeout=180,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
        if x.returncode:raise RuntimeError(f'{name}: exit {x.returncode}')
        return (out/name).read_text()

    baseline=subprocess.check_output(['git','show',BASE+':arch/x86_64/elf.c'],cwd=ROOT)
    (out/'original-elf.c').write_bytes(baseline)
    body=re.search(r'static int inside\([^}]+\}',baseline.decode())[0]
    assert 'return off <= size && len <= size - off;' in body
    assert subprocess.check_output(['git','show',BASE+':arch/x86_64/elf.h'],cwd=ROOT)==(ROOT/'arch/x86_64/elf.h').read_bytes()
    flags=['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
           '-fno-pic','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables',
           '-mno-sse','-mno-mmx','-m64','-mcmodel=kernel','-mno-red-zone',
           '-mgeneral-regs-only','-fcf-protection=none','-fno-stack-clash-protection',
           '-fno-lto','-Iarch/x86_64']
    cc=['gcc',*flags]
    run(cc+['-Delf_validate=bench_elf_c','-Delf_strerror=bench_error_c','-c',out/'original-elf.c','-o',out/'elf-c.o'],'compile-original.txt')
    run(cc+['-DVOS_ELF_USE_RUST','-Delf_validate=bench_elf_rust','-Delf_strerror=bench_error_rust',
            '-c','arch/x86_64/elf.c','-o',out/'elf-rust.o'],'compile-rust-caller.txt')
    run(cc+['-c','tests/bench/elf-bounds/predicates.c','-o',out/'predicates.o'],'compile-predicates.txt')
    run(cc+['-c','tests/bench/elf-bounds/runner.c','-o',out/'runner.o'],'compile-runner.txt')
    run(['gcc','-fno-pie','-no-pie','-fno-lto',out/'runner.o',out/'predicates.o',
         out/'elf-c.o',out/'elf-rust.o',archive,'-o',out/'bench'],'link.txt')
    assembly={}
    for symbol in ['bench_c_inside','vos_rust_elf_inside','bench_call_control',
                   'bench_predicate_abi','bench_predicate_inline','bench_validate','bench_elf_c','bench_elf_rust']:
        assembly[symbol]=run(['objdump','-d','--disassemble='+symbol,out/'bench'],symbol+'.asm.txt')
    assert re.search(r'call\s+\*',assembly['bench_predicate_abi'])
    assert re.search(r'call\s+\*',assembly['bench_validate'])
    assert 'vos_rust_elf_inside' not in assembly['bench_elf_c']
    assert 'vos_rust_elf_inside' in assembly['bench_elf_rust']
    symbol_text=run(['nm','-S',out/'bench'],'symbols.txt')
    sizes={s:int(re.search(r'^[0-9a-f]+ ([0-9a-f]+) T '+s+r'$',symbol_text,re.M)[1],16) for s in assembly}
    corpus=out/'corpus';corpus.mkdir();raw=(user/'hello.elf').read_bytes()
    valid=[user/(n+'.elf') for n in ['hello','vsh','rixie','gui-demo','pointer-demo','hello','vsh','rixie']]
    bad=[]
    for i,length in enumerate([64,128,200,280]):
        path=corpus/f'truncated-{i}.elf';path.write_bytes(raw[:length]);bad.append(path)
    for off in [len(raw)-8,len(raw),len(raw)+8,0x100000]:
        data=bytearray(raw);data[32:40]=off.to_bytes(8,'little')
        path=corpus/f'phoff-{off}.elf';path.write_bytes(data);bad.append(path)
    other=[]
    phoff=int.from_bytes(raw[32:40],'little');phnum=int.from_bytes(raw[56:58],'little')
    note=next(phoff+i*56 for i in range(phnum) if int.from_bytes(raw[phoff+i*56:phoff+i*56+4],'little')==4)
    note_off=int.from_bytes(raw[note+8:note+16],'little')
    for i,(offset,width,value) in enumerate([(0,1,0),(4,1,1),(18,2,0),(16,2,3),
                                           (note_off+16,4,6),(phoff+4,4,7),(24,8,0),(phoff+32,8,1<<40)]):
        data=bytearray(raw);data[offset:offset+width]=value.to_bytes(width,'little')
        path=corpus/f'other-{i}.elf';path.write_bytes(data);other.append(path)
    groups=[valid,bad,other,valid[:4]+bad[:2]+other[:2]]
    (out/'corpus-list.txt').write_text(''.join(str(path)+'\n' for group in groups for path in group))
    corpus_info={name:[{'path':str(path),'bytes':path.stat().st_size,'sha256':sha(path)} for path in group]
                 for name,group in zip(['valid','bounds_reject','other_reject','mixed'],groups)}
    os.sched_setaffinity(0,{cpu}) # Only this dedicated driver and its children.
    info={'uname':platform.uname()._asdict(),'cpuinfo':Path('/proc/cpuinfo').read_text(),
          'allowed_cpus_before':allowed,'pinned_cpu':cpu,'exclusive_cpu':False,
          'load_before':os.getloadavg(),'C_version':subprocess.check_output(['gcc','--version'],text=True),
          'linker_version':subprocess.check_output(['ld','--version'],text=True),'C_flags':flags,
          'link_flags':['-fno-pie','-no-pie','-fno-lto'],'Rust_archive_sha256':sha(archive),
          'baseline_source_sha256':sha(out/'original-elf.c'),'baseline_commit':BASE,
          'samples':a.samples,'scalar_operations_per_sample':a.scalar_ops,'ELF_operations_per_sample':65536,
          'vector_positions':256,'vector_bytes':256*24,'sanitizers':False,
          'power_frequency_policy':'unchanged/uncontrolled','method':'WSL2 hosted; no QEMU timing; no physical VOS performance claim'}
    rust_inputs=archive.with_name('inputs')
    info['Rust_compiler_flags_and_sysroot_record']=rust_inputs.read_text()
    info['Rust_input_record_sha256']=sha(rust_inputs)
    (out/'rust-inputs.txt').write_bytes(rust_inputs.read_bytes())
    binary=out/'bench';argv=[binary,'0x564f535231',str(a.samples),str(a.scalar_ops),out/'corpus-list.txt']
    with (out/'raw.csv').open('xb') as stdout,(out/'run.txt').open('xb') as stderr:
        x=subprocess.run(list(map(str,argv)),cwd=ROOT,stdout=stdout,stderr=stderr,timeout=180)
    commands.append(list(map(str,argv)))
    info['load_after']=os.getloadavg();info['exit_status']=x.returncode
    (out/'environment.json').write_text(json.dumps(info,indent=2)+'\n')
    (out/'corpus.json').write_text(json.dumps(corpus_info,indent=2)+'\n')
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    if x.returncode:raise RuntimeError('benchmark failed preflight or checksum; see run.txt')
    rows=list(csv.DictReader((out/'raw.csv').open()));summary={}
    for key in sorted({(x['comparison'],x['implementation']) for x in rows}):
        selected=[x for x in rows if (x['comparison'],x['implementation'])==key];entry={'samples':len(selected)}
        for clock in ['wall','cpu']:
            values=sorted(int(x[clock+'_ns'])/int(x['operations']) for x in selected)
            entry[clock+'_ns_per_op']={'median':statistics.median(values),'min':min(values),'max':max(values),
                                     'p10':values[int((len(values)-1)*.1)],'p90':values[int((len(values)-1)*.9)]}
        entry['involuntary_switches']=sum(int(x['involuntary_switches']) for x in selected)
        summary['/'.join(key)]=entry
    (out/'summary.json').write_text(json.dumps({'sizes':sizes,'measurements':summary},indent=2)+'\n')
    print((out/'run.txt').read_text());print(json.dumps({'sizes':sizes,'measurements':summary},indent=2))


if __name__=='__main__':main()

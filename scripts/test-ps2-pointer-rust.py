#!/usr/bin/env python3
"""R2-B actual-production-archive differential and affected input checks.

Fresh output only; no cleanup, legacy admission code or historical mutations.
Rust's bare-metal archive is not sanitizer-instrumented; the C oracle/harness
and affected C service/renderer are ASan/UBSan-instrumented separately.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-','_'), ROOT/'scripts'/name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--archive',type=Path,required=True)
    ap.add_argument('--rustc',required=True)
    ap.add_argument('--sanitizer-evidence',type=Path,help='reuse unchanged differential inputs from an earlier successful sanitizer step')
    args=ap.parse_args()
    out=args.out.resolve();archive=args.archive.resolve()
    assert out.is_relative_to(ROOT/'build') and not out.exists() and archive.is_file()
    out.mkdir(parents=True)
    h=load('test-usb-probe.py'); run=h.run
    results={'passed':False,'archive_sha256':h.sha(archive),'rust_sanitized':False}
    try:
        common=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
        source='tests/host/ps2-pointer-differential.c'
        for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']),
                           ('release',['-O2','-mno-red-zone','-mgeneral-regs-only','-fno-lto'])]:
            if mode=='sanitizer' and args.sanitizer_evidence:
                prior=args.sanitizer_evidence.resolve()
                previous=json.loads((prior/'result.json').read_text())
                assert previous['archive_sha256']==h.sha(archive)
                text=(prior/'sanitizer.log').read_text()
                assert '52394348 byte/idle comparisons' in text
                results['sanitizer']=text
                results['sanitizer_reused_from']=str(prior)
                continue
            ref=out/(mode+'-reference.o');binary=out/(mode+'-differential')
            run(common+flags+['-Dpointer_decode=pointer_decode_reference','-Dpointer_idle=pointer_idle_reference',
                            '-c','arch/x86_64/pointer.c','-o',ref],out/(mode+'-reference-build.log'))
            # The benchmark formatter uses host floating point, not the decoder.
            # Keep the C reference's kernel-like scalar flags independently.
            harness_flags=[f for f in flags if f!='-mgeneral-regs-only']
            run(common+harness_flags+[source,ref,archive,'-o',binary],out/(mode+'-build.log'))
            results[mode]=run([binary],out/(mode+'.log'))
            if mode=='release':
                # Benchmark a single host CPU where available, never sanitizer timings.
                command=[binary,'--bench']
                if hasattr(os,'sched_getaffinity'):
                    cpu=min(os.sched_getaffinity(0));command=['taskset','-c',str(cpu),*command]
                    results['benchmark_cpu']=cpu
                results['performance']=run(command,out/'performance.csv')
        run([args.rustc,'--edition=2024','--test','-Dwarnings','-Copt-level=2','-Cdebuginfo=0',
             '-Cstrip=debuginfo','tests/rust/ps2_pointer.rs','-o',out/'rust-tests'],out/'rust-build.log')
        results['rust']=run([out/'rust-tests'],out/'rust-tests.log')
        service=out/'service';service.mkdir()
        results['regressions']=h.hosted(service,args.rustc,archive)
        debug=service/'debug.c'
        flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        run(common+flags+['-DVOS_GUI_HOST','-DVOS_CONSOLE_HOST','tests/host/pointer.c',
            'arch/x86_64/gui.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c',
            'arch/x86_64/input_console.c',debug,archive,'-o',out/'pointer-service'],out/'pointer-service-build.log')
        results['pointer_service']=run([out/'pointer-service'],out/'pointer-service.log')
        run(common+flags+['-DPOINTER_RENDER','tests/host/pointer.c','arch/x86_64/desktop.c',
             'arch/x86_64/font8x16.c','-o',out/'pointer-render'],out/'pointer-render-build.log')
        results['renderer']=run([out/'pointer-render'],out/'pointer-render.log')
        results['passed']=True
    finally:
        with (out/'result.json').open('x') as f:json.dump(results,f,indent=2);f.write('\n')
    print(json.dumps({k:v for k,v in results.items() if k!='performance'},indent=2))

if __name__=='__main__':main()

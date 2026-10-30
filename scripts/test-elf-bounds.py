#!/usr/bin/env python3
"""R1/A1 focused host checks; use a fresh output root and an already-built kernel archive.

A1: elf_validate and elf_strerror are Rust (kernel/rust/elf/validate.rs, arch/x86_64/rust/elf_validate.rs) and come from the
kernel archive; arch/x86_64/elf.c is the byte-identical frozen C reference. This script compares the two (tests/host/elf-compare.c
and tests/host/elf-differential.c, the second also with the same Rust sources built for the host with arithmetic overflow
checks and abort-on-panic), proves the differential detects deliberately broken references, and keeps the R1 checks.

Example: make ARCH=x86_64 BUILD=build/r1/kernel USER_BUILD=build/r1/user
ELFCHECK=build/r1/elfcheck RUSTC=/path/to/rustc user-test
then run this script with the corresponding --archive, --elfcheck and --user.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--elfcheck', type=Path, required=True)
    p.add_argument('--user', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--rustc', default=os.environ.get('RUSTC'), help='default: $RUSTC')
    a = p.parse_args()
    if not a.rustc:
        p.error('--rustc or RUSTC is required (the hosted overflow-checked build of the validator)')
    archive, checker, user, out = [x.resolve() for x in (a.archive,a.elfcheck,a.user,a.out)]
    if out.exists():
        p.error('output already exists: preserve it and choose a fresh path')
    symbols=subprocess.check_output(['nm',str(checker)],text=True)
    if ' T elf_validate\n' not in symbols or ' T elf_validate_reference\n' in symbols:
        p.error('--elfcheck must be the Make-built checker that links the kernel archive, not the C reference')
    if ' T elf_validate\n' not in subprocess.check_output(['nm',str(archive)],text=True) or \
       ' T elf_strerror\n' not in subprocess.check_output(['nm',str(archive)],text=True):
        p.error('--archive does not define the Rust elf_validate and elf_strerror')
    out.mkdir(parents=True)
    result = {'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
              'primary_checker_links_the_archive':True,'archive_defines_elf_validate_and_elf_strerror':True}

    def run(args, name, expect=0):
        with (out/name).open('xb') as stream:
            r = subprocess.run(list(map(str,args)),cwd=ROOT,stdout=stream,
                               stderr=subprocess.STDOUT,timeout=60,
                               env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
        if r.returncode!=expect:
            raise RuntimeError(f'{name}: exit {r.returncode}, expected {expect}')
        return (out/name).read_text()

    cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-Iarch/x86_64']
    san=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    try:
        run(cc+san+['-Wl,--gc-sections','tests/host/elf-bounds.c',archive,'-o',out/'bounds'],'bounds-build.txt')
        result['bounds']=run([out/'bounds'],'bounds.txt').strip()
        # The frozen C reference under other names; the primary validator is the Rust one in the archive.
        run(cc+san+['-Delf_validate=elf_validate_reference','-Delf_strerror=elf_strerror_reference',
                    '-c','arch/x86_64/elf.c','-o',out/'reference.o'],'reference-build.txt')
        run(cc+san+['-Wl,--gc-sections','tests/host/elf-compare.c',out/'reference.o',archive,
                    '-o',out/'compare'],'compare-build.txt')
        # Keep the pre-existing standalone C-only compiler invocation supported.
        run(cc+['-O2','scripts/elfcheck.c','arch/x86_64/elf.c','-o',out/'elfcheck-reference'],'reference-tool-build.txt')
        raw=(user/'hello.elf').read_bytes();corpus=out/'corpus';corpus.mkdir()
        inputs=sorted(user.rglob('*.elf'))
        for name,data in [('empty',b''),('header-only',raw[:64]),('truncated-ph',raw[:200])]:
            path=corpus/(name+'.elf');path.write_bytes(data);inputs.append(path)
        for value in [0,8,63,64,len(raw),len(raw)+8,(1<<40)-1,1<<40,(1<<64)-1]:
            data=bytearray(raw);data[32:40]=value.to_bytes(8,'little')
            path=corpus/(f'phoff-{value}.elf');path.write_bytes(data);inputs.append(path)
        rng=random.Random(0x564f535231)
        for i in range(128):
            data=bytearray(raw);index=rng.randrange(len(data));data[index]^=1<<rng.randrange(8)
            path=corpus/(f'bit-{i:03}.elf');path.write_bytes(data);inputs.append(path)
        result['validators']=run([out/'compare',*inputs],'compare.txt').strip()
        bases=[x for x in sorted(user.rglob('*.elf'))]
        run(cc+san+['-Wl,--gc-sections','tests/host/elf-differential.c',out/'reference.o',archive,
                    '-o',out/'differential'],'differential-build.txt')
        diff=run([out/'differential',*bases],'differential.txt').strip().splitlines()
        result['differential_archive_build']=diff[0]
        result['differential_outcomes']=diff[1:]
        tmp=Path(tempfile.mkdtemp(prefix='elf-a1-'))
        try:
            run([a.rustc,'--edition=2024','-Dwarnings','--crate-type=staticlib','--crate-name','elf_host','-Cpanic=abort',
                 '-Coverflow-checks=yes','-Cdebug-assertions=yes','-Copt-level=1','tests/rust/elf_validate_host.rs',
                 '-o',tmp/'libelf_host.a'],'host-rust-build.txt')
            run(cc+san+['-Wl,--gc-sections','tests/host/elf-differential.c',out/'reference.o',tmp/'libelf_host.a',
                        '-o',tmp/'differential-host'],'differential-host-build.txt')
            host=run([tmp/'differential-host',*bases],'differential-host.txt').strip().splitlines()
            result['differential_overflow_checked_host_build']=host[0]
            # The differential must detect a deliberately broken C reference (a one-line change each): a mismatch, or a
            # fault of the guard pages when the broken reference reads outside the image. Sources are never modified.
            ref_src=(ROOT/'arch/x86_64/elf.c').read_text()
            wx=("            if (flags & ~(ELF_PF_R | ELF_PF_W | ELF_PF_X) || !(flags & ELF_PF_R))\n                return ELF_EFLAGS;\n"
                "            if ((flags & ELF_PF_W) && (flags & ELF_PF_X))\n                return ELF_EWX;")
            wx_swapped=("            if ((flags & ELF_PF_W) && (flags & ELF_PF_X))\n                return ELF_EWX;\n"
                        "            if (flags & ~(ELF_PF_R | ELF_PF_W | ELF_PF_X) || !(flags & ELF_PF_R))\n                return ELF_EFLAGS;")
            mutants=[('filesz-ge-memsz','filesz > memsz ||','filesz >= memsz ||'),
                     ('user-end-inclusive','vaddr + memsz > USER_END','vaddr + memsz >= USER_END'),
                     ('user-start-inclusive','vaddr < USER_START ||','vaddr <= USER_START ||'),
                     ('entry-inclusive','entry < vaddr + filesz','entry <= vaddr + filesz'),
                     ('entry-start','entry >= vaddr &&','entry > vaddr &&'),
                     ('e-version','le(img, 20, 4) != 1','le(img, 20, 4) != 2'),
                     ('note-desc-size','descsz != 4 ||','descsz != 8 ||'),
                     ('note-padding','(namesz + 3) & ~3ULL','(namesz + 7) & ~3ULL'),
                     ('note-loop','while (end - n >= 12)','while (end - n > 12)'),
                     ('note-bound','if (na + da > end - n - 12)','if (na + da >= end - n - 12)'),
                     ('flags-wx-order',wx,wx_swapped),
                     ('image-pages-boundary','r.pages > ELF_MAX_IMAGE_PAGES','r.pages >= ELF_MAX_IMAGE_PAGES'),
                     ('overlap-boundary','vaddr < prev_end','vaddr <= prev_end'),
                     ('phdr-table-bound','len <= size - off','len < size - off'),
                     ('field-limit','off >= FIELD_LIMIT ||','off > FIELD_LIMIT ||'),
                     ('phnum-limit','phnum > ELF_MAX_PHNUM','phnum >= ELF_MAX_PHNUM'),
                     ('size-minimum','size < 64 ||','size < 63 ||'),
                     ('message-text','bad ELF magic','bad ELF magik'),
                     ('image-not-zeroed-on-failure','    *out = r;\n    if (size < 64','    if (size < 64')]
            controls={}
            for name,old,new in mutants:
                if ref_src.count(old)!=1 or old==new:
                    raise RuntimeError(f'differential control {name}: anchor occurs {ref_src.count(old)} times')
                (tmp/'mutant.c').write_text(ref_src.replace(old,new))
                subprocess.run(cc+['-O1','-Delf_validate=elf_validate_reference','-Delf_strerror=elf_strerror_reference',
                                   '-c',tmp/'mutant.c','-o',tmp/'mutant.o'],cwd=ROOT,check=True,capture_output=True)
                subprocess.run(cc+['-O1','-Wl,--gc-sections','tests/host/elf-differential.c',tmp/'mutant.o',archive,
                                   '-o',tmp/'mutant-differential'],cwd=ROOT,check=True,capture_output=True)
                m=subprocess.run([tmp/'mutant-differential',*bases],cwd=tmp,capture_output=True,text=True,timeout=120)
                how=('mismatch' if m.returncode==1 and ('MISMATCH' in m.stderr or 'elf_strerror' in m.stderr)
                     else 'guard-page fault' if m.returncode<0 else 'NOT DETECTED')
                controls[name]=how
            if any(v=='NOT DETECTED' for v in controls.values()):
                raise RuntimeError(f'the differential missed a broken reference: {controls}')
            result['differential_detects_broken_references']=controls
        finally:
            shutil.rmtree(tmp,ignore_errors=True)
        for path in inputs:
            x=subprocess.run([str(checker),str(path)],cwd=ROOT,capture_output=True,timeout=5)
            y=subprocess.run([str(out/'elfcheck-reference'),str(path)],cwd=ROOT,capture_output=True,timeout=5)
            if x.returncode not in (0,1) or (x.returncode,x.stdout,x.stderr)!=(y.returncode,y.stdout,y.stderr):
                raise RuntimeError(f'elfcheck Rust/C mismatch: {path}')
        result['host_tool_exact_output_comparisons']=len(inputs)
        # A safe, fail-closed mutation must be detected by both test layers.
        deny=out/'deny.c'
        deny.write_text('#include <stdint.h>\nint vos_rust_elf_inside(uint64_t s,uint64_t o,uint64_t n) '
                        '{(void)s;(void)o;(void)n;return 0;}\n')
        run(cc+['-O2','tests/host/elf-bounds.c',deny,'-o',out/'deny-bounds'],'deny-bounds-build.txt')
        run([out/'deny-bounds'],'deny-bounds.txt',expect=1)
        run(cc+['-O2','-DVOS_ELF_USE_RUST','scripts/elfcheck.c','arch/x86_64/elf.c',deny,
                '-o',out/'deny-checker'],'deny-checker-build.txt')
        text=run([out/'deny-checker',user/'hello.elf'],'deny-checker.txt',expect=1)
        assert 'program-header table outside the file' in text
        result['fail_closed_mutation_controls']='2/2 PASS: deny-all cannot pass bounds vectors or accept valid hello'
        result['corpus_sha256']={str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path):
                                 hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
        result['status']='PASS'
    except BaseException as error:
        result['status']='FAIL';result['error']=str(error)
        raise
    finally:
        (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='corpus_sha256'},indent=2))


if __name__=='__main__':
    main()

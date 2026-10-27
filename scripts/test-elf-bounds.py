#!/usr/bin/env python3
"""R1 focused host checks; use a fresh output root and an already-built kernel archive.

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
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--elfcheck', type=Path, required=True)
    p.add_argument('--user', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    archive, checker, user, out = [x.resolve() for x in (a.archive,a.elfcheck,a.user,a.out)]
    if out.exists():
        p.error('output already exists: preserve it and choose a fresh path')
    symbols=subprocess.check_output(['nm',str(checker)],text=True)
    if ' T vos_rust_elf_inside\n' not in symbols:
        p.error('--elfcheck must be the Make-built Rust-backed checker, not the C reference')
    out.mkdir(parents=True)
    result = {'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
              'primary_checker_Rust_symbol':True}

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
        # Compile the same validator twice, changing only its selected predicate.
        run(cc+san+['-DVOS_ELF_USE_RUST','-c','arch/x86_64/elf.c','-o',out/'rust.o'],'rust-validator-build.txt')
        run(cc+san+['-Delf_validate=elf_validate_reference','-Delf_strerror=elf_strerror_reference',
                    '-c','arch/x86_64/elf.c','-o',out/'reference.o'],'reference-build.txt')
        run(cc+san+['-Wl,--gc-sections','tests/host/elf-compare.c',out/'rust.o',out/'reference.o',archive,
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

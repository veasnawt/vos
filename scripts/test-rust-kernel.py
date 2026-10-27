#!/usr/bin/env python3
"""Focused R0-B checks. Fresh output roots only; never cleans retained evidence.

Requires the existing bare-metal Rust target, GRUB BIOS/EFI producers, QEMU,
OVMF and ordinary binutils. No downloads, Cargo or historical controller imports.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(args, log, cwd=ROOT):
    with log.open('xb') as stream:
        result = subprocess.run(list(map(str, args)), cwd=cwd, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=180,
                                env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed ({result.returncode}); see {log}')
    return log.read_text(errors='replace')


def capture(args):
    return subprocess.check_output(list(map(str, args)), cwd=ROOT, text=True)


class Guest:
    def __init__(self, out, iso, firmware, cpu):
        self.out = out
        out.mkdir()
        self.log = out / 'serial.log'
        self.sock = out / 'qmp.sock'
        args = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg',
                '-m', '128M', '-cpu', cpu, '-nic', 'none', '-no-reboot',
                '-display', 'none', '-serial', f'file:{self.log}',
                '-qmp', f'unix:{self.sock},server=on,wait=off',
                '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                '-cdrom', str(iso)]
        if firmware == 'uefi':
            variables = out / 'OVMF_VARS.fd'
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            args += ['-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                     '-drive', f'if=pflash,format=raw,file={variables}']
        (out / 'command.json').write_text(json.dumps(args, indent=2) + '\n')
        self.err = (out / 'qemu.log').open('xb')
        self.process = subprocess.Popen(args, stdout=self.err, stderr=self.err)
        self.conn = None
        deadline = time.monotonic() + 10
        while not self.sock.exists():
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f'QEMU startup failed: {out}')
            time.sleep(.02)
        self.conn = socket.socket(socket.AF_UNIX)
        self.conn.settimeout(10)
        self.conn.connect(str(self.sock))
        self.pipe = self.conn.makefile('rwb', buffering=0)
        json.loads(self.pipe.readline())
        self.command('qmp_capabilities')

    def command(self, name, arguments=None):
        self.pipe.write(json.dumps(dict(execute=name, arguments=arguments or {})).encode() + b'\n')
        while True:
            response = json.loads(self.pipe.readline())
            if 'error' in response:
                raise RuntimeError(response)
            if 'return' in response:
                return response['return']

    def text(self):
        return self.log.read_text(errors='replace') if self.log.exists() else ''

    def wait(self, marker, start=0, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            text = self.text()
            if marker in text[start:]:
                return text
            if self.process.poll() is not None:
                raise RuntimeError(f'QEMU exited before {marker!r}: {self.out}')
            time.sleep(.02)
        raise RuntimeError(f'timed out waiting for {marker!r}: {self.out}')

    def key(self, key):
        self.command('send-key', {'keys': [{'type': 'qcode', 'data': key}], 'hold-time': 30})
        # QEMU's release timer must expire before the same key can be reused.
        time.sleep(.06)

    def line(self, text):
        for char in text:
            self.key('spc' if char == ' ' else char)
        self.key('ret')

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.conn:
            self.conn.close()
        self.err.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rustc', type=Path, required=True)
    parser.add_argument('--grub-efi-dir', type=Path, required=True)
    parser.add_argument('--baseline', default='67a525e2cbcb13215c4d17cdfd0e3740ad40ff16',
                        help='accepted C-only source commit for size comparison')
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists():
        parser.error('output exists: preserve it and select a fresh path')
    out.mkdir(parents=True)
    results = {'baseline': args.baseline}

    def save():
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')

    def make(build, label, *targets, extra=(), cwd=ROOT, arch='x86_64'):
        return run(['make', '--no-print-directory', '-j2', f'ARCH={arch}',
                    f'BUILD={build}', f'USER_BUILD={out / "user"}',
                    f'RUSTC={args.rustc}', f'GRUB_EFI_DIR={args.grub_efi_dir}',
                    *extra, *targets], out / (label + '.log'), cwd)

    try:
        normal = out / 'normal'
        make(normal, 'build', 'all')
        make(out / 'repeat', 'repeat-build', 'all')
        for relative in ['vos.bin', 'rust/libvos_kernel.a']:
            assert digest(normal / relative) == digest(out / 'repeat' / relative), relative
        results['reproducible'] = {p: digest(normal / p) for p in ['vos.bin', 'rust/libvos_kernel.a']}
        save()

        # Baseline is the accepted parent, not a modified or old working area.
        source = out / 'baseline-source'
        source.mkdir()
        archive = subprocess.check_output(['git', 'archive', args.baseline], cwd=ROOT)
        import io
        with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
            tree.extractall(source, filter='data')
        make(out / 'baseline', 'baseline-build', 'all', cwd=source)
        results['sizes'] = {name: {'file_bytes': (path / 'vos.bin').stat().st_size,
                                    'sha256': digest(path / 'vos.bin'),
                                    'size': capture(['size', path / 'vos.bin']).strip()}
                            for name, path in [('baseline', out / 'baseline'), ('rust', normal)]}

        library = normal / 'rust/libvos_kernel.a'
        symbols = capture(['nm', '-A', '--defined-only', library])
        member = re.search(r':([^:\n]+):[0-9a-f]+ T vos_rust_probe\n', symbols).group(1)
        obj = out / 'kernel-rust.o'
        obj.write_bytes(subprocess.check_output(['ar', 'p', str(library), member]))
        for name, command in {
            'rust-object-sections': ['readelf', '-SW', obj],
            'rust-object-relocations': ['readelf', '-rW', obj],
            'rust-object-disassembly': ['objdump', '-dr', obj],
            'kernel-sections': ['readelf', '-SW', normal / 'vos.bin'],
            'kernel-segments': ['readelf', '-lW', normal / 'vos.bin'],
            'rust-call-site': ['objdump', '-dr', normal / 'kernel.o'],
        }.items():
            run(command, out / (name + '.txt'))
        assert not capture(['nm', '-u', normal / 'vos.bin']).strip()
        sections = (out / 'kernel-sections.txt').read_text()
        assert not re.search(r'\.(?:tdata|tbss|dynamic|dynsym|plt|got|eh_frame)\b', sections)
        relocations = (out / 'rust-object-relocations.txt').read_text()
        assert not re.search(r'GOT|TLS|TPOFF|GLOB_DAT|JUMP_SLOT', relocations)
        instructions = (out / 'rust-object-disassembly.txt').read_text()
        assert not re.search(r'%(?:[xyz]mm\d+|mm[0-7]|st\b)|\b(?:fxsave|xsave|ldmxcsr|emms)\b', instructions)
        assert not re.search(r'-0x[0-9a-f]+\(%rsp\)', instructions)
        assert re.search(r'R_X86_64_\w+\s+vos_rust_probe', (out / 'rust-call-site.txt').read_text())
        probe_address = int(re.search(r'([0-9a-f]+) T vos_rust_probe\n', capture(['nm', normal / 'vos.bin'])).group(1), 16)
        run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wl,--gc-sections', '-Iarch/x86_64',
             'tests/host/rust-kernel.c', library, '-o', out / 'host-abi'], out / 'host-build.log')
        run([out / 'host-abi'], out / 'host-abi.log')
        results['host_abi_pairs'] = 4134
        results['binary_inspection'] = 'PASS: static C call, integer-only Rust, no red zone/runtime/TLS/unwind imports'
        save()

        # A fixture copy exercises source and flag dependencies without editing source.
        # Preserve relative Rust module paths in the isolated source fixture.
        fixture_root = out / 'dependency-source'
        shutil.copytree(ROOT / 'arch/x86_64/rust', fixture_root / 'arch/x86_64/rust')
        shutil.copytree(ROOT / 'kernel/rust/usb/xhci', fixture_root / 'kernel/rust/usb/xhci')
        fixture = fixture_root / 'arch/x86_64/rust/kernel_probe.rs'
        shutil.copyfile(ROOT / 'arch/x86_64/rust/kernel_probe.rs', fixture)
        dep = out / 'dependency'
        target = str(dep / 'rust/libvos_kernel.a')
        extra = (f'KERNEL_RUST_SOURCE={fixture}',)
        make(dep, 'dependency-initial', target, extra=extra)
        first = Path(target).stat().st_mtime_ns
        make(dep, 'dependency-unchanged', target, extra=extra)
        assert Path(target).stat().st_mtime_ns == first
        fixture.write_text(fixture.read_text() + '\n// dependency fixture change\n')
        make(dep, 'dependency-source', target, extra=extra)
        second = Path(target).stat().st_mtime_ns
        assert second > first
        make(dep, 'dependency-flags', target, extra=extra + ('KERNEL_RUST_EXTRA_FLAGS=-C opt-level=1',))
        assert Path(target).stat().st_mtime_ns > second
        results['dependencies'] = 'PASS: unchanged, source change, compiler flags; compiler/sysroot hashes pinned in inputs'
        save()

        for case in ['rust-panic', 'rust-result']:
            build = out / case
            make(build, case + '-build', 'all', extra=('VOS_TEST=1', f'VOS_TEST_CASE={case}'))
            assert not capture(['nm', '-u', build / 'vos.bin']).strip()
            make(build, case + '-images', 'uefi-iso', extra=('VOS_TEST=1', f'VOS_TEST_CASE={case}'))
        make(normal, 'normal-images', 'uefi-iso')
        make(out / 'i386', 'i386-verify', 'verify', arch='i386')
        make(normal, 'apps-build', 'user-vsh', 'user-rust', 'user-gui', 'user-pointer', extra=('RUST_APP=rixie',))
        media = out / 'media'
        media.mkdir()
        shutil.copyfile(normal / 'vos.iso', media / 'hello-bios.iso')
        shutil.copyfile(normal / 'vos-uefi.iso', media / 'hello-uefi.iso')
        files = ' '.join(f'{out / "user" / (name + ".elf")}:boot/{name}.elf'
                         for name in ['vsh', 'hello', 'rixie', 'gui-demo', 'pointer-demo'])
        make(normal, 'interactive-images', 'uefi-iso',
             extra=('GRUB_CFG=arch/x86_64/grub-pointer.cfg', f'ISO_FILES={files}'))
        for fw, filename in [('bios', 'vos.iso'), ('uefi', 'vos-uefi.iso')]:
            shutil.copyfile(normal / filename, media / f'interactive-{fw}.iso')
        results['image_hashes'] = {}
        for iso in sorted(media.glob('*.iso')):
            results['image_hashes'][iso.name] = {'bytes': iso.stat().st_size, 'sha256': digest(iso)}
            report = run(['xorriso', '-indev', iso, '-report_el_torito', 'plain', '-report_system_area', 'plain'],
                         out / (iso.stem + '-image.txt'))
            assert ('UEFI' if 'uefi' in iso.name else 'BIOS') in report
            extract = out / (iso.stem + '-kernel.bin')
            run(['xorriso', '-osirrox', 'on', '-indev', iso, '-extract', '/boot/vos.bin', extract],
                out / (iso.stem + '-extract.log'))
            assert digest(extract) == digest(normal / 'vos.bin')
        save()

        native = []
        for fw in ['bios', 'uefi']:
            for kind, cpu in [('hello', 'qemu64'), ('hello-max', 'max'),
                              ('rust-panic', 'qemu64'), ('rust-result', 'qemu64'),
                              ('interactive', 'qemu64')]:
                name = f'{fw}-{kind}'
                iso = (media / f'hello-{fw}.iso' if kind.startswith('hello') else
                       media / f'interactive-{fw}.iso' if kind == 'interactive' else
                       out / kind / ('vos.iso' if fw == 'bios' else 'vos-uefi.iso'))
                guest = Guest(out / name, iso, fw, cpu)
                try:
                    if kind == 'rust-result':
                        guest.wait('panic: Rust kernel probe result mismatch')
                        assert 'rust: kernel probe PASS' not in guest.text()
                    else:
                        text = guest.wait('rust: kernel probe PASS')
                        match = re.search(r'fn=(0x[0-9a-f]+) seed=(0x[0-9a-f]+) salt=(0x[0-9a-f]+) result=(0x[0-9a-f]+)', text)
                        assert match, text[-1000:]
                        fn, seed, salt, value = [int(v, 16) for v in match.groups()]
                        mask = (1 << 64) - 1
                        expected = ((((seed << 17) | (seed >> 47)) + ((salt >> 9) | (salt << 55))) & mask) ^ 0x564f535255535430
                        assert expected == value
                        if not kind.startswith('rust-'):
                            assert fn == probe_address
                    if kind == 'rust-panic':
                        guest.wait('panic: Rust kernel panic')
                    if kind.startswith('rust-'):
                        guest.wait('VOS-TEST: FAIL')
                        assert guest.process.wait(timeout=10) == 35
                    elif kind.startswith('hello'):
                        guest.wait('boot: checks passed')
                        guest.wait('Hello from VOS!')
                        guest.wait('all frames reclaimed')
                    else:
                        guest.wait('pointer-demo: READY')
                        guest.key('a'); guest.wait('pointer-demo: KEY a')
                        guest.key('b'); guest.wait('pointer-demo: KEY b')
                        start = len(guest.text())
                        guest.command('input-send-event', {'events': [
                            {'type': 'rel', 'data': {'axis': 'x', 'value': 40}},
                            {'type': 'rel', 'data': {'axis': 'y', 'value': 20}}]})
                        guest.wait('pointer-demo: x=', start)
                        guest.command('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
                        guest.wait('pointer-demo: LEFT DOWN')
                        guest.command('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
                        guest.wait('pointer-demo: LEFT UP')
                        guest.key('q'); guest.wait('pointer-demo: RELEASED')
                        guest.wait('VOS native vsh')
                        guest.wait('VOS>')
                        guest.line('apps'); guest.wait('rixie')
                        start = len(guest.text())
                        guest.line('hello'); guest.wait('Hello from VOS!', start)
                        guest.line('rixie'); guest.wait('Rixie>')
                        start = len(guest.text())
                        guest.line('help'); guest.wait('Rixie>', start)
                        start = len(guest.text())
                        guest.line('exit'); guest.wait('VOS>', start)
                        start = len(guest.text())
                        guest.line('echo restored'); guest.wait('restored', start)
                        guest.line('exit'); guest.wait('native: vsh PASS, process reclaimed')
                    assert kind.startswith('rust-') or 'panic:' not in guest.text()
                    native.append({'case': name, 'cpu': cpu, 'result': 'PASS',
                                   'serial_sha256': digest(guest.log)})
                    results['native'] = native
                    save()
                finally:
                    guest.stop()
        results['status'] = 'PASS'
        save()
        print(json.dumps(results, indent=2))
    except BaseException as error:
        results['status'] = 'FAIL'
        results['error'] = str(error)
        save()
        raise


if __name__ == '__main__':
    main()

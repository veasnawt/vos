#!/usr/bin/env python3
"""Focused N4 host + BIOS/UEFI keyboard check. Fresh output only; no host disks.

Uses real QMP key events and production code, not a serial-input substitute.
Run with python3 -B. Existing N3 build trees and evidence are never inputs to make.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import socket
import struct
import subprocess
import time
import zlib

REPO = Path(__file__).resolve().parents[1]


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def run(argv, log, timeout=120):
    env = os.environ.copy()
    for key in ('MAKEFLAGS', 'MFLAGS', 'MAKEFILES', 'GNUMAKEFLAGS', 'MAKELEVEL', 'CC', 'CFLAGS', 'LDFLAGS'):
        env.pop(key, None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    with log.open('x') as out:
        subprocess.run(argv, cwd=REPO, stdout=out, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout, env=env)


class QMP:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(2)
        self.socket.connect(str(path))
        self.file = self.socket.makefile('rwb', buffering=0)
        require('QMP' in json.loads(self.file.readline()), 'missing QMP greeting')
        self.execute('qmp_capabilities')

    def execute(self, name, arguments=None):
        cmd = {'execute': name}
        if arguments is not None:
            cmd['arguments'] = arguments
        self.file.write(json.dumps(cmd).encode()+b'\n')
        while True:
            row = json.loads(self.file.readline())
            require('error' not in row, 'QMP error: '+str(row))
            if 'return' in row:
                return row['return']

    def key(self, code, down):
        self.execute('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}
        ]})

    def tap(self, code):
        self.key(code, True)
        self.key(code, False)
        time.sleep(.025)

    def close(self):
        self.file.close()
        self.socket.close()


def png_from_ppm(source, target):
    """Lossless format conversion of QEMU screendump, without retouching pixels."""
    with source.open('rb') as f:
        require(f.readline().strip() == b'P6', 'unsupported screendump format')
        width, height = map(int, f.readline().split())
        require(f.readline().strip() == b'255', 'unsupported screendump depth')
        pixels = f.read()
    require(len(pixels) == width*height*3, 'truncated screenshot')
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    raw = b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    with target.open('xb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))+
                chunk(b'IDAT', zlib.compress(raw))+chunk(b'IEND', b''))
    return {'width': width, 'height': height, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}


def terminal_lines(data):
    """Normalize only CR/newline and BS used by the real echo path."""
    lines = ['']
    for c in data.decode('ascii', errors='replace'):
        if c == '\n':
            lines.append('')
        elif c == '\b':
            lines[-1] = lines[-1][:-1]
        elif c != '\r':
            lines[-1] += c
    return lines


def boot(out, firmware, iso, interactive=True, buffered=False):
    prefix = out/firmware
    serial = prefix.with_suffix('.serial.log')
    qmp_path = out/(firmware+'.qmp')
    args = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M',
            '-cdrom', str(iso), '-boot', 'd', '-display', 'none', '-nic', 'none',
            '-serial', 'file:'+str(serial), '-qmp', 'unix:'+str(qmp_path)+',server=on,wait=off',
            '-no-reboot', '-no-shutdown']
    if firmware == 'uefi':
        code = Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
        var_source = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')
        require(code.is_file() and var_source.is_file(), 'OVMF 4M firmware missing')
        variables = out/'OVMF_VARS_4M.fd'
        require(not variables.exists(), 'OVMF VARS already exists')
        shutil.copyfile(var_source, variables)
        args += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file='+str(code),
                 '-drive', 'if=pflash,format=raw,unit=1,file='+str(variables)]
    (out/(firmware+'-command.json')).write_text(json.dumps(args, indent=2)+'\n')
    qmp = None
    with prefix.with_suffix('.qemu.log').open('x') as log:
        proc = subprocess.Popen(args, cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic()+45
            marker = b'input> ' if interactive else b'boot: checks passed'
            while True:
                require(proc.poll() is None, 'QEMU exited during boot')
                raw = serial.read_bytes() if serial.exists() else b''
                require(not any(x in raw for x in (b'panic:', b'boot: FAILED', b'check failed:', b'VOS-TEST: FAIL')),
                        'guest failure; see '+str(serial))
                if marker in raw and qmp_path.exists():
                    break
                require(time.monotonic() < deadline, 'boot deadline expired')
                time.sleep(.05)
            qmp = QMP(qmp_path)
            require(b'user[hello]: Hello from VOS!' in raw and
                    b'proc[hello]: exited status 0, all frames reclaimed' in raw, 'ABI v0 hello regression')
            if not interactive:
                # Preserve the historical normal boot/return-to-halt behavior.
                regs = qmp.execute('human-monitor-command', {'command-line': 'info registers'})
                require('HLT=1' in regs, 'normal boot no longer halts')
                require(b'input: PS/2 ready' not in raw, 'console enabled without exact token')
                return {'passed': True, 'boot_halted': True, 'hello_abi_v0': True}
            require(b'input: PS/2 ready' in raw, 'keyboard initialization missing')
            # Prompt boundary, ordinary text, Shift, erase, Caps and ignored arrow/Ctrl.
            qmp.tap('backspace')
            qmp.key('shift', True)
            for c in 'vos': qmp.tap(c)
            qmp.key('shift', False)
            qmp.tap('spc'); qmp.tap('x'); qmp.tap('backspace')
            qmp.tap('caps_lock'); qmp.tap('o'); qmp.tap('k'); qmp.tap('caps_lock')
            qmp.tap('up')
            qmp.key('ctrl', True); qmp.tap('c'); qmp.key('ctrl', False)
            if buffered:
                time.sleep(.1)
                require(b'submitted: ' not in serial.read_bytes(), 'partial line reached consumer before Enter')
            qmp.tap('ret')
            for c in 'hello': qmp.tap(c)
            qmp.tap('spc')
            for c in 'vos': qmp.tap(c)
            qmp.tap('ret')
            deadline = time.monotonic()+5
            while True:
                raw = serial.read_bytes()
                lines = terminal_lines(raw)
                if 'input> VOS OK' in lines and 'input> hello vos' in lines and raw.count(b'input> ') >= 3:
                    break
                require(time.monotonic() < deadline, 'keyboard echo/edit verdict missing')
                time.sleep(.05)
            require(not any(x in raw for x in (b'panic:', b'boot: FAILED')), 'guest failed after input')
            ppm, png = prefix.with_suffix('.ppm'), prefix.with_suffix('.png')
            qmp.execute('screendump', {'filename': str(ppm)})
            shot = png_from_ppm(ppm, png)
            result = {'passed': True, 'hello_abi_v0': True, 'edited_line': 'input> VOS OK',
                      'second_line': 'input> hello vos', 'screenshot': shot}
            if buffered:
                require(b'submitted: VOS OK\r\n' in raw and b'submitted: hello vos\r\n' in raw,
                        'edited/second completed line not delivered')
                # Empty line, exact-capacity editing, overflow discard and next-line recovery.
                qmp.tap('backspace'); qmp.tap('ret')
                for _ in range(128): qmp.tap('a')
                qmp.tap('backspace'); qmp.tap('z'); qmp.tap('ret')
                for _ in range(129): qmp.tap('x')
                qmp.tap('backspace'); qmp.tap('ret')
                for c in 'ready for vsh': qmp.tap('spc' if c==' ' else c)
                qmp.tap('ret')
                for i in range(6):
                    for c in 'line '+str(i+1): qmp.tap('spc' if c==' ' else c)
                    qmp.tap('ret')
                deadline = time.monotonic()+8
                while True:
                    raw = serial.read_bytes()
                    if b'submitted: line 6\r\ninput> ' in raw:
                        break
                    require(time.monotonic()<deadline, 'buffered input delivery deadline')
                    time.sleep(.05)
                lines = raw.split(b'\r\n')
                require(b'submitted: ' in lines, 'empty line not delivered')
                require(b'submitted: '+b'a'*127+b'z' in lines, 'exact-limit edit/copy mismatch')
                require(raw.count(b'input: line too long; discarded (128 characters maximum)')==1,
                        'overflow was not explicitly rejected once')
                require(not any(x.startswith(b'submitted: x') for x in lines), 'truncated overflow delivered')
                require(b'submitted: ready for vsh' in lines, 'reader failed to recover after overflow')
                require(not any(x in raw for x in (b'panic:',b'boot: FAILED',b'VOS-TEST: FAIL')),
                        'guest failed after buffered input')
                qmp.execute('screendump', {'filename': str(out/(firmware+'-limits.ppm'))})
                limits = png_from_ppm(out/(firmware+'-limits.ppm'),out/(firmware+'-limits.png'))
                result.update(completed_line_delivery=True,no_partial_delivery=True,empty_line=True,
                              exact_limit_edited=True,overflow_discarded=True,recovery=True,
                              chronological_output_exercised=True,limits_screenshot=limits)
            return result
        finally:
            if qmp:
                qmp.close()
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=3)
            # No process-tree kill: this child alone belongs to this test.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='fresh directory under build/')
    parser.add_argument('--buffered', action='store_true', help='verify completed-line delivery and limits')
    args = parser.parse_args()
    out = args.output.resolve()
    require(out.is_relative_to(REPO/'build') and out != REPO/'build', 'output must be a child of build/')
    require(not out.exists(), 'refusing to overwrite a previous result')
    out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (64*1024**2, 64*1024**2))
    result = {'scope': 'N4 buffered command input' if args.buffered else 'first N4 keyboard-to-console increment', 'runs': {}}
    try:
        common = ['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
                  '-fdata-sections', '-Wl,--gc-sections', '-Iarch/x86_64']
        run(common+['tests/host/keyboard-input.c', 'arch/x86_64/keyboard.c', 'arch/x86_64/input_console.c',
                    '-o', str(out/'keyboard-host')], out/'host-build.log')
        run([str(out/'keyboard-host')], out/'host-test.log', timeout=10)
        run(common+['tests/host/screen-input.c', 'arch/x86_64/font8x16.c',
                    '-o', str(out/'screen-host')], out/'screen-build.log')
        run([str(out/'screen-host')], out/'screen-test.log', timeout=10)
        if args.buffered:
            run(common+['tests/host/buffered-input.c','arch/x86_64/input_console.c',
                        '-o',str(out/'buffered-host')], out/'buffered-build.log')
            run([str(out/'buffered-host')],out/'buffered-test.log',timeout=10)
        result['host'] = 'PASS'
        run(['make', '--no-print-directory', 'input-uefi-iso', 'INPUT_BUILD='+str(out/'image')], out/'image-build.log')
        kernel = out/'image/kernel'
        result['runs']['bios'] = boot(out, 'bios', kernel/'vos.iso',buffered=args.buffered)
        result['runs']['uefi'] = boot(out, 'uefi', kernel/'vos-uefi.iso',buffered=args.buffered)
        # Same new kernel, original GRUB config; distinct output tree, no N3 artifacts touched.
        run(['make', '--no-print-directory', 'ARCH=x86_64', 'BUILD='+str(out/'normal'),
             'USER_BUILD='+str(out/'image/user'), 'iso'], out/'normal-build.log')
        result['runs']['normal'] = boot(out, 'normal', out/'normal/vos.iso', interactive=False)
        result['passed'] = True
    except BaseException as exc:
        result.update(passed=False, error=str(exc))
        raise
    finally:
        (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
        entries = []
        for path in sorted(out.rglob('*')):
            if path.is_file():
                entries.append({'path': str(path.relative_to(out)), 'size': path.stat().st_size,
                                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        (out/'manifest.json').write_text(json.dumps(entries, indent=2)+'\n')
        print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Gate A only: verify a sealed preboot admission, then run one UEFI canary.

No builds, full SC groups, cleanup, fixture regeneration or automatic retries.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from storage_stress_host import Supervisor, MIB, require
from uefi_catalog import validate_iso


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--attempt', type=int, choices=[1, 2], required=True)
    args = parser.parse_args()
    s = Supervisor(args.evidence)
    r = s.root
    receipt = json.loads((r / 'preboot.json').read_text())
    require(receipt['production_isolation'] and receipt['fixture_reproducible'] and
            receipt['configuration'] == dict(firmware='UEFI', machine='q35', cpu='max', memory='128M'),
            'preboot gates not satisfied')
    for rel, expected in receipt['sha256'].items():
        p = r / rel
        require(p.resolve().is_relative_to(r) and digest(p) == expected, 'preboot provenance mismatch: ' + rel)
    catalog_env = json.loads((r / 'build-env.json').read_text())
    catalog = validate_iso(r / receipt['iso'], r / 'preboot-iso-validation', s, catalog_env)
    require(catalog['iso_sha256'] == receipt['sha256'][receipt['iso']],
            'validated ISO differs from admitted ISO')
    out = r / ('canary-' + str(args.attempt))
    require(not out.exists(), 'attempt already exists')
    s.guard(8 * MIB)
    out.mkdir()
    s.copy(r / 'inputs/OVMF_VARS_4M.fd', out / 'vars.fd')
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-cpu', 'max', '-m', '128M',
            '-drive', 'if=pflash,format=raw,readonly=on,file=' + str(r / 'inputs/OVMF_CODE_4M.fd'),
            '-drive', 'if=pflash,format=raw,file=' + str(out / 'vars.fd'),
            '-cdrom', str(r / receipt['iso']), '-boot', 'd',
            '-drive', 'file=' + str(r / receipt['fixture']) + ',if=none,id=vd0,format=raw,readonly=on',
            '-device', 'virtio-blk-pci,drive=vd0,disable-legacy=on',
            '-display', 'none', '-no-reboot', '-nic', 'none',
            '-serial', 'file:' + str(out / 'serial.log'),
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
            '-d', 'cpu_reset,guest_errors', '-D', str(out / 'qemu.log')]
    result = dict(configuration=receipt['configuration'], argv=argv, timeout=240,
                  groups_run=[], passed=False)
    try:
        s.run('canary-' + str(args.attempt), ['timeout', '--kill-after=2', '240', *argv],
              extra=8 * MIB, timeout=245, expected=33)
        serial = (out / 'serial.log').read_text(errors='strict')
        trusted = [line for line in serial.splitlines() if not line.startswith('user[')]
        markers = [
            'storage-canary: ABI calls=13 scopes=8 reads=2 wire=64/272 EOF preserved PASS',
            'storage-canary: exact frames/identities/owners restored PASS',
            'storage-canary: SC01 SC02 SC03 SC04 SC05 SC10 SC11 NOT RUN',
            'storage-canary: GATE A PASS', 'VOS-TEST: PASS']
        for marker in markers:
            require(trusted.count(marker) == 1, 'missing/duplicate trusted marker: ' + marker)
        request = [re.fullmatch(r'storage-canary: genuine polling requests=([1-9][0-9]*) completions/retirements exact PASS', x) for x in trusted]
        request = [x for x in request if x]
        checks = [re.fullmatch(r'storage-canary: checks=([1-9][0-9]*) PASS', x) for x in trusted]
        checks = [x for x in checks if x]
        require(len(request) == len(checks) == 1, 'missing exact trusted count records')
        require(not any(re.search(r'FAILED|FAIL:|VOS-TEST: FAIL|panic:|check failed', x) for x in trusted), 'guest failure record')
        require('Triple fault' not in (out / 'qemu.log').read_text(), 'triple fault')
        for rel, expected in receipt['sha256'].items():
            require(digest(r / rel) == expected, 'input changed during QEMU: ' + rel)
        result.update(passed=True, exit_code=33, checks=int(checks[0][1]),
                      real_requests=int(request[0][1]), markers=markers,
                      serial_sha256=digest(out / 'serial.log'))
    finally:
        with (out / 'result.json').open('x') as f:
            f.write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

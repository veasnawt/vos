#!/usr/bin/env python3
"""R3-N1 CI: the accepted host ARP/IPv4/UDP/TCP protocol suites, built under ASan/UBSan with the C
forwarders and the Rust network core. Fresh confined output; no QEMU or network access."""
import argparse
import json
import runpy
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SUITES = [('net-arp', []), ('net-ipv4', ['arch/x86_64/net_ipv4.c']), ('net-udp', ['arch/x86_64/net_ipv4.c', 'arch/x86_64/net_udp.c']),
          ('net-tcp', ['arch/x86_64/net_ipv4.c', 'arch/x86_64/net_tcp.c']), ('net-tcp-data', ['arch/x86_64/net_ipv4.c', 'arch/x86_64/net_tcp.c']),
          ('net-tcp-retry', ['arch/x86_64/net_ipv4.c', 'arch/x86_64/net_tcp.c']), ('net-tcp-close', ['arch/x86_64/net_ipv4.c', 'arch/x86_64/net_tcp.c'])]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    out = ap.parse_args().out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        raise SystemExit('fresh output under build/ required')
    out.mkdir(parents=True)
    lib = runpy.run_path(str(ROOT / 'scripts/net_rust_host.py'), run_name='net_rust_host')['build'](out)
    cc = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
          '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-Iarch/x86_64']
    result = {}
    for name, extra in SUITES:
        binary = out / name
        with (out / (name + '-build.log')).open('x') as f:
            subprocess.run(cc + ['tests/host/' + name + '.c', 'arch/x86_64/net_arp.c', 'arch/x86_64/net_boot.c'] + extra + [lib, '-o', str(binary)],
                           cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, check=True)
        p = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True, timeout=900)
        (out / (name + '-test.log')).write_text(p.stdout + p.stderr)
        if p.returncode or 'PASS' not in p.stdout:
            raise SystemExit('host suite failed: ' + name)
        result[name] = p.stdout.strip().splitlines()[-1]
    (out / 'result.json').write_text(json.dumps(dict(status='PASS', suites=result), indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()

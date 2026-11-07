#!/usr/bin/env python3
"""R3-N1: host static library of the Rust ARP/IPv4/UDP core for the host protocol suites.
net_arp.c, net_ipv4.c and net_udp.c forward to this core, so every host build that compiles them
links the library returned by build(). Used through runpy by the existing network scripts."""
import json
import os
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def build(out):
    out = Path(out)
    root = out / 'net-rust-root.rs'
    lib = out / 'libnet-rust.a'
    if lib.exists():
        return str(lib)
    root.write_text('#![no_std]\n#[path=' + json.dumps(str(ROOT / 'arch/x86_64/rust/net.rs')) + '] mod net;\n'
                    'unsafe extern "C" {fn abort()->!;}\n'
                    '#[panic_handler] fn net_host_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
    rustc = os.environ.get('RUSTC', 'rustc')
    with (out / 'net-rust-build.log').open('x') as log:
        subprocess.run([rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Coverflow-checks=yes', '--crate-type=staticlib',
                        '-Cpanic=abort', str(root), '-o', str(lib)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    return str(lib)

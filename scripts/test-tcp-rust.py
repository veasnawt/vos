#!/usr/bin/env python3
"""R3-N2 Rust TCP: the actual kernel archive against the frozen accepted C TCP (f8731c9) in two configurations
(A: over the frozen R3-N1 C ARP/IPv4; B: over the production Rust IPv4), a whole-arena three-way differential in
release, ASan/UBSan and checked-Rust builds, safe-core tests, named mutation controls that the differential must
reject, entry frames against the accepted C (gcc on this host), panic freedom, the memory-symbol set (no memmove)
and symbol ownership. Fresh confined outputs only; no cleanup or historical evidence changes.
"""
import argparse, csv, hashlib, io, json, os, re, shutil, statistics, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
COVERAGE = 'PASS 374029 TCP comparisons'
SEQUENCES = 'and 4000 random sequences'
TCP = ['net_tcp_init', 'net_tcp_input', 'net_tcp_expire', 'net_tcp_close', 'net_tcp_enable_payload', 'net_tcp_send', 'net_tcp_receive',
       'net_tcp_ack', 'net_tcp_abort', 'net_tcp_enable_retransmission', 'net_tcp_retransmit', 'net_tcp_enable_closure', 'net_tcp_finish']
ENTRY = {n: 'vos_rust_' + n for n in TCP}
NET_PUBLIC = ['net_checksum', 'net_ipv4_transport_checksum', 'net_ethernet_type', 'net_ipv4_payload', 'net_ipv4_begin', 'net_arp_init',
              'net_neighbor_get', 'net_arp_query', 'net_ethernet_input', 'net_ipv4_init', 'net_ipv4_echo_request', 'net_ipv4_input',
              'net_udp_init', 'net_udp_send', 'net_udp_input', 'net_udp_receive', 'net_udp_close']
# Entry-frame exceptions PENDING source-specific owner approval: {public name: Rust bytes allowed}. The Rust entries hold the
# 118-byte segment buffer that the C keeps in its callees (packet/fin_packet); whole-path stack depth is equal or lower for
# every public function under gcc 13 and gcc 15 (docs/results/r3-n2-tcp-rust.md). Not approved by this file.
FRAME_EXCEPTIONS = {'net_tcp_send': 280, 'net_tcp_retransmit': 248, 'net_tcp_finish': 200}
MEMORY_SYMBOLS = {'memcpy', 'memset', 'memcmp'}
# Equivalent mutants excluded after analysis (no API-reachable state distinguishes them from the accepted C):
#  data/FIN retry budget `>= RETRY_MAX` -> `>`: the absolute TX TTL (200 >= 50*(3+1)) always expires first;
#  `room = actual` clamp: rcv_adv - rcv_next never exceeds 64 - rx_used (recomputed at every emitted segment);
#  window-update `(seq != win_seq || ack >= win_ack)`: a current ACK is never older than win_ack;
#  `fin || received` zero window: data after the peer FIN never passes the sequence test.
MUTATIONS = [  # (name, exact text in kernel/rust/net/tcp.rs, replacement); the differential must FAIL for each
 ('init-port', '    if port == 0 {\n        return EINVAL;\n    }\n    let mut ipctx', '    if port == 1 {\n        return EINVAL;\n    }\n    let mut ipctx'),
 ('init-isn', 'c.next_isn = initial_sequence;', 'c.next_isn = initial_sequence.wrapping_add(1);'),
 ('init-state', '    c.state = LISTEN;\n    0\n}', '    c.state = SYN_RECEIVED;\n    0\n}'),
 ('abort-guard', '    if c.port == 0 {\n        return;\n    }\n    c.peer = [0; 4];', '    c.peer = [0; 4];'),
 ('abort-peer', '    c.peer = [0; 4];\n    c.peer_mac = [0; 6];\n    c.peer_port = 0;', '    c.peer_mac = [0; 6];\n    c.peer_port = 0;'),
 ('abort-adv', '    c.rcv_adv = 0;\n', ''),
 ('abort-tx', '    c.tx = [0; TX_MAX];\n}', '}'),
 ('close-zero', '    *c = Tcp::ZERO;\n}', '    c.port = 0;\n}'),
 ('enable-retx-needs-payload', '    if c.port == 0 || c.payload_enabled == 0 {\n        return EINVAL;\n    }\n    if c.state != LISTEN {\n        return BUSY;\n    }\n    c.retry_enabled = 1;',
  '    if c.port == 0 {\n        return EINVAL;\n    }\n    if c.state != LISTEN {\n        return BUSY;\n    }\n    c.retry_enabled = 1;'),
 ('enable-closure-busy', '    if c.state != LISTEN {\n        return BUSY;\n    }\n    c.closure_enabled = 1;', '    c.closure_enabled = 1;'),
 ('idle-ttl', 'now.wrapping_sub(c.since) >= IDLE_TTL', 'now.wrapping_sub(c.since) > IDLE_TTL'),
 ('payload-tx-ttl', '(c.tx_used != 0 && now.wrapping_sub(c.tx_since) >= TX_TTL)', '(c.tx_used != 0 && now.wrapping_sub(c.tx_since) > TX_TTL)'),
 ('close-ttl', 'now.wrapping_sub(c.since) >= CLOSE_TTL', 'now.wrapping_sub(c.since) > CLOSE_TTL'),
 ('hard-ttl', '>= CLOSE_HARD_TTL', '> CLOSE_HARD_TTL'),
 ('fin-pending-ttl', '((c.tx_used != 0 || fin_pending(c)) && now', '((c.tx_used != 0) && now'),
 ('time-wait-ttl', '>= TIME_WAIT_TTL', '> TIME_WAIT_TTL'),
 ('time-wait-rx', ' && c.rx_used == 0 {', ' {'),
 ('expire-handshake', '(c.state == SYN_RECEIVED && now.wrapping_sub(c.since) >= HANDSHAKE_TTL)', '(c.state == SYN_RECEIVED && now.wrapping_sub(c.since) > HANDSHAKE_TTL)'),
 ('ready-expired', '        return if payload_timeout(c, now) { EXPIRED } else { 0 };', '        return 0;'),
 ('shift-short', '    while i < keep {', '    while i + 1 < keep {'),
 ('shift-offset', 'p.get(i.wrapping_add(consumed))', 'p.get(i.wrapping_add(consumed).wrapping_add(1))'),
 ('shift-tail', '        *d = 0;\n        j += 1;', '        *d = 1;\n        j += 1;'),
 ('packet-window', 'put16(&mut f[48..50], window as u16);', 'put16(&mut f[48..50], (window as u16).wrapping_add(1));'),
 ('packet-psh', 'f[47] = if n != 0 { 24 } else { 16 };', 'f[47] = 16;'),
 ('packet-ports', '    put16(&mut f[34..36], c.port);\n    put16(&mut f[36..38], c.peer_port);', '    put16(&mut f[34..36], c.peer_port);\n    put16(&mut f[36..38], c.port);'),
 ('packet-min-wire', 'let wire = if 54 + n < 60 { 60 } else { 54 + n };', 'let wire = 54 + n;'),
 ('packet-checksum-span', 'Some(h) => transport_checksum(&c.ip.arp.ip, &c.peer, 6, h),', 'Some(h) => transport_checksum(&c.ip.arp.ip, &c.peer, 6, h.get(..20).unwrap_or(h)),'),
 ('fin-flags', 's.frame[47] = 17;', 's.frame[47] = 1;'),
 ('fin-checksum', '    put16(&mut s.frame[50..52], sum);\n    emitted(c, size, s);\n    0', '    let _ = sum;\n    emitted(c, size, s);\n    0'),
 ('emit-identification', '    c.ip.identification = c.ip.identification.wrapping_add(1);\n    c.rcv_adv', '    c.rcv_adv'),
 ('emit-advertised', '    c.rcv_adv = c.rcv_next.wrapping_add(receive_window(c));', '    c.rcv_adv = c.rcv_next.wrapping_add(RX_MAX as u32);'),
 ('emit-dirty', '    c.window_dirty = 0;\n}', '}'),
 ('window-closed', 'if c.closure_enabled != 0 && peer_finished(c) { 0 } else {', 'if c.closure_enabled != 0 && c.state == TIME_WAIT { 0 } else {'),
 ('send-busy', '    if c.tx_used != 0 {\n        return BUSY;\n    }\n    if n > c.peer_window as usize {', '    if n > c.peer_window as usize {'),
 ('send-window', '    if n > c.peer_window as usize {\n        return WINDOW;', '    if n > c.peer_window as usize + 1 {\n        return WINDOW;'),
 ('send-close-wait', 'if c.state != ESTABLISHED && c.state != CLOSE_WAIT {\n        return EINVAL;\n    }\n    let Some(data)', 'if c.state != ESTABLISHED {\n        return EINVAL;\n    }\n    let Some(data)'),
 ('send-retry-after', 'c.retry_after = if c.retry_enabled != 0 { RETRY_TICKS } else { 0 };', 'c.retry_after = RETRY_TICKS;'),
 ('send-since', '    c.tx_since = now;\n    if c.state == ESTABLISHED {\n        c.since = now;\n    }\n    c.retry_count = 0;', '    c.tx_since = now;\n    c.retry_count = 0;'),
 ('retx-window-clip', '        n = c.peer_window as usize;', '        n = c.tx_used as usize;'),
 ('retx-sequence', 'Words { seq: c.snd_una, ack: c.rcv_next, window: receive_window(c) }, data, s)', 'Words { seq: c.snd_next, ack: c.rcv_next, window: receive_window(c) }, data, s)'),
 ('retx-expired-abort', '    if e == EXPIRED {\n        tcp_abort(c);\n        return e;\n    }', '    if e == EXPIRED {\n        return e;\n    }'),
 ('retx-due', '    let elapsed = now.wrapping_sub(c.tx_since);\n    if elapsed < c.retry_after as u64 {', '    let elapsed = now.wrapping_sub(c.tx_since);\n    if elapsed <= c.retry_after as u64 {'),
 ('retx-after', '    c.retry_after = (elapsed as u32).wrapping_add(RETRY_TICKS);\n    emitted(c, size, s);', '    c.retry_after = elapsed as u32;\n    emitted(c, size, s);'),
 ('retx-fin-sequence', 'let e = fin_packet(c, c.snd_una, s);', 'let e = fin_packet(c, c.snd_next, s);'),
 ('receive-eof', 'Err(if c.closure_enabled != 0 && peer_finished(c) { EOF } else { EMPTY })', 'Err(if c.closure_enabled != 0 && peer_finished(c) { EMPTY } else { EOF })'),
 ('receive-capacity', '    if n > capacity {\n        n = capacity;\n    }', '    if n > capacity {\n        n = capacity - 1;\n    }'),
 ('receive-dirty', '    c.window_dirty = 1;', '    c.window_dirty = 0;'),
 ('payload-sequence', '        seq == c.rcv_next && n as u64 <= room as u64\n    } else if room != 0 {', '        n as u64 <= room as u64\n    } else if room != 0 {'),
 ('payload-room-edge', '    } else if room != 0 {\n        seq.wrapping_sub(c.rcv_next) < room\n    } else {\n        seq == c.rcv_next\n    };\n    if !sequence_ok {\n        return tcp_ack(c, now, s);\n    }\n    let mut size = 0;\n    if n != 0 {',
  '    } else if room != 0 {\n        seq.wrapping_sub(c.rcv_next) <= room\n    } else {\n        seq == c.rcv_next\n    };\n    if !sequence_ok {\n        return tcp_ack(c, now, s);\n    }\n    let mut size = 0;\n    if n != 0 {'),
 ('payload-old-ack', '    if !ack_current && c.snd_una.wrapping_sub(ack) >= 0x8000_0000 {\n        return DROP;\n    }\n    let mut room', '    if !ack_current && c.snd_una.wrapping_sub(ack) > 0x8000_0000 {\n        return DROP;\n    }\n    let mut room'),
 ('payload-window-since', '            if window != c.peer_window {\n                c.since = now;\n            }\n', ''),
 ('payload-ack-window', 'window: actual.wrapping_sub(n as u32) }', 'window: actual }'),
 ('payload-retry-rearm', '            } else if c.retry_enabled != 0 {\n                c.retry_after = (now.wrapping_sub(c.tx_since) as u32).wrapping_add(RETRY_TICKS);\n            }\n        }\n        if seq.wrapping_sub(c.win_seq) < 0x8000_0000 && (seq != c.win_seq || ack.wrapping_sub(c.win_ack) < 0x8000_0000) {\n            let window',
  '            } else if c.retry_enabled != 0 {\n                c.retry_after = now.wrapping_sub(c.tx_since) as u32;\n            }\n        }\n        if seq.wrapping_sub(c.win_seq) < 0x8000_0000 && (seq != c.win_seq || ack.wrapping_sub(c.win_ack) < 0x8000_0000) {\n            let window'),
 ('rst-exact', '        if seq == c.rcv_next {\n            tcp_abort(c);\n            return RESET;', '        if seq.wrapping_sub(c.rcv_next) < 2 {\n            tcp_abort(c);\n            return RESET;'),
 ('rst-time-wait', '        if n != 0 || c.state == TIME_WAIT {\n            return DROP;', '        if n != 0 {\n            return DROP;'),
 ('rst-challenge', '        if room != 0 && seq.wrapping_sub(c.rcv_next) < room {\n            return tcp_ack(c, now, s);', '        if room != 0 && seq.wrapping_sub(c.rcv_next) <= room {\n            return tcp_ack(c, now, s);'),
 ('closure-flags', 'if flags != 16 && flags != 24 && flags != 17 && flags != 25 {', 'if flags != 16 && flags != 24 && flags != 17 {'),
 ('duplicate-fin', 'seq.wrapping_add(n as u32) == c.rcv_next.wrapping_sub(1)', 'seq.wrapping_add(n as u32) == c.rcv_next'),
 ('fin-window-edge', '    if fin && !duplicate_fin && n as u64 == room as u64 {', '    if fin && !duplicate_fin && n as u64 == room as u64 + 1 {'),
 ('last-ack-done', '                if c.state == LAST_ACK {\n                    tcp_abort(c);\n                    return DONE;', '                if c.state == LAST_ACK {\n                    c.state = TIME_WAIT;\n                    return DONE;'),
 ('closing-time-wait', '                if c.state == CLOSING {\n                    time_wait(c, now);', '                if c.state == CLOSING {\n                    c.state = TIME_WAIT;'),
 ('fin-wait-2-clock', '                    c.state = FIN_WAIT_2;\n                    c.tx_since = 0;', '                    c.state = FIN_WAIT_2;'),
 ('closure-window', '            c.peer_window = get16(h, 14);\n            c.win_seq = seq;', '            c.peer_window = get16(h, 14).wrapping_add(1);\n            c.win_seq = seq;'),
 ('close-wait-since', '            c.state = CLOSE_WAIT;\n            c.since = now;', '            c.state = CLOSE_WAIT;'),
 ('fin-consumes-sequence', '        c.rcv_next = c.rcv_next.wrapping_add(1);\n        if c.state == ESTABLISHED {', '        if c.state == ESTABLISHED {'),
 ('time-wait-refresh', '        if c.state == TIME_WAIT {\n            c.tx_since = now;\n        }\n', ''),
 ('header-reserved', 'h[12] & 15 != 0', 'h[12] & 7 != 0'),
 ('header-urgent', 'if header != 20 || get16(h, 18) != 0 {', 'if header != 20 {'),
 ('header-checksum', '        || transport_checksum(&source, &c.ip.arp.ip, 6, tcp) != 0 {', '        {'),
 ('header-source-port', '|| get16(h, 0) == 0 ||', '||'),
 ('data-profile', '(c.state != ESTABLISHED && !(c.closure_enabled != 0 && c.state >= CLOSE_WAIT))', '(c.state != ESTABLISHED)'),
 ('port-drop', '    if get16(h, 2) != c.port {\n        return DROP;\n    }\n', ''),
 ('peer-mac', 'if peer_port != c.peer_port || source != c.peer || peer_mac != c.peer_mac {\n            return DROP;\n        }\n        if c.state == SYN_RECEIVED', 'if peer_port != c.peer_port || source != c.peer {\n            return DROP;\n        }\n        if c.state == SYN_RECEIVED'),
 ('syn-rst-sequence', 'if data_length != 0 || seq != c.rcv_next {', 'if data_length != 0 {'),
 ('listen-ack-field', 'if flags != 2 || ack != 0 {', 'if flags != 2 {'),
 ('listen-window', 'if c.payload_enabled != 0 { RX_MAX as u16 } else { 0 }', 'RX_MAX as u16'),
 ('listen-identification', '        c.ip.identification = c.ip.identification.wrapping_add(1);\n        if c.payload_enabled != 0 {', '        if c.payload_enabled != 0 {'),
 ('listen-ack-number', 'put32(f, 42, seq.wrapping_add(1));', 'put32(f, 42, seq);'),
 ('listen-isn-step', 'c.next_isn = c.next_isn.wrapping_add(0x10000);', 'c.next_isn = c.next_isn.wrapping_add(0x1000);'),
 ('syn-advertised', 'c.rcv_adv = c.rcv_next.wrapping_add(RX_MAX as u32);', 'c.rcv_adv = c.rcv_next;'),
 ('connect-ack', '|| seq != c.rcv_next || ack != c.snd_next {', '|| seq != c.rcv_next {'),
 ('connect-window', '        c.peer_window = get16(h, 14);\n        c.win_seq = seq;\n        c.win_ack = ack;\n        c.since = now;\n    }\n    CONNECTED', '        c.win_seq = seq;\n        c.win_ack = ack;\n        c.since = now;\n    }\n    CONNECTED'),
 ('handshake-ttl', '            if now.wrapping_sub(c.since) >= HANDSHAKE_TTL {\n                return EXPIRED;', '            if now.wrapping_sub(c.since) > HANDSHAKE_TTL {\n                return EXPIRED;'),
 ('delegate-ipv4', 'Err(UNSUPPORTED) => return TcpIn::Ipv4Input,', 'Err(UNSUPPORTED) if false => return TcpIn::Ipv4Input,'),
]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x, s):
    if not x: raise RuntimeError(s)
def frame(asm, name):
    """Entry frame: 8 * every push plus every rsp allocation in the body. `sub $-N,%rsp` (a deallocation) is not counted
    and `add $-N,%rsp` (an allocation, gcc's imm8 form of sub $N) is; argument pushes for calls are included."""
    m = re.search(r'^[0-9a-f]+ <' + re.escape(name) + r'>:\n(.*?)(?=\n\n|\Z)', asm, re.M | re.S); need(m, 'function ' + name)
    b = m[1]
    subs = [int(x, 16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp', b)]
    adds = [int(x, 16) for x in re.findall(r'\tadd\s+\$0x([0-9a-f]+),%rsp', b)]
    return 8 * len(re.findall(r'\tpush\s', b)) + sum(x for x in subs if x < 1 << 63) + sum((1 << 64) - x for x in adds if x >= 1 << 63)
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True); ap.add_argument('--rustc', required=True); ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--skip-mutations', action='store_true', help='development only; a PASS requires every mutation control')
    a = ap.parse_args()
    out = a.out.resolve(); archive = a.archive.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build') and archive.is_file(), 'fresh confined output'); out.mkdir(parents=True)
    report = {'status': 'RUNNING', 'archive_sha256': sha(archive), 'commands': [], 'seed': '52334e3254435021'}
    def run(cmd, name, expect=0, timeout=1500, check=True):
        cmd = list(map(str, cmd)); row = {'argv': cmd, 'log': name}; report['commands'].append(row)
        with (out / name).open('x') as f: p = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = p.returncode
        if check: need(p.returncode == expect, 'unexpected exit ' + name)
        return (out / name).read_text()
    try:
        for manifest, src, hdr in (('net-r3n1.json', 'tests/reference', 'tests/reference/net-headers'), ('net-r3n2.json', 'tests/reference', 'tests/reference/net-headers')):
            ref = json.loads((ROOT / 'tests/reference' / manifest).read_text())
            for n, v in ref['sources_sha256'].items(): need(sha(ROOT / src / n) == v, 'frozen accepted source ' + n)
            for n, v in ref['headers_sha256'].items(): need(sha(ROOT / hdr / n) == v, 'frozen header ' + n)
        report['oracle'] = {'baseline': json.loads((ROOT / 'tests/reference/net-r3n2.json').read_text())['baseline'], 'tcp_source_sha256': sha(ROOT / 'tests/reference/net-tcp.c'),
                            'tcp_header_sha256': sha(ROOT / 'tests/reference/net-headers/net_tcp.h')}
        cc = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Iarch/x86_64', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-fno-pie', '-no-pie']
        refcc = [*cc[:5], '-Itests/reference/net-headers', *cc[5:]]
        net_ref = ['-D%s=%s_reference' % (n, n) for n in NET_PUBLIC]
        cfg = {'A': net_ref + ['-D%s=%s_rA' % (n, n) for n in TCP], 'B': ['-D%s=%s_rB' % (n, n) for n in TCP]}
        kernel = ['-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-mno-red-zone', '-mgeneral-regs-only', '-mcmodel=kernel']
        report['gcc'] = subprocess.run(['gcc', '--version'], capture_output=True, text=True).stdout.splitlines()[0]
        objs = {}
        for mode, flags in [('sanitizer', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']), ('release', ['-O2', '-fno-lto'])]:
            scalar = flags + (kernel if mode == 'release' else []); objs[mode] = []
            for f in ('arp', 'ipv4'):
                o = out / ('%s-reference-%s.o' % (mode, f)); run(refcc + scalar + net_ref + ['-c', 'tests/reference/net-%s.c' % f, '-o', o], o.name + '.txt'); objs[mode].append(o)
            for c in ('A', 'B'):
                o = out / ('%s-reference-tcp-%s.o' % (mode, c)); run(refcc + scalar + cfg[c] + ['-c', 'tests/reference/net-tcp.c', '-o', o], o.name + '.txt'); objs[mode].append(o)
            for f in ('arp', 'ipv4', 'tcp'):
                o = out / ('%s-driver-%s.o' % (mode, f)); run(cc + scalar + ['-c', 'arch/x86_64/net_%s.c' % f, '-o', o], o.name + '.txt'); objs[mode].append(o)
            run(cc + flags + ['tests/host/tcp-differential.c', *objs[mode], archive, '-o', out / (mode + '-differential')], mode + '-build.txt')
            report[mode] = run([out / (mode + '-differential')], mode + '.txt')
            need(COVERAGE in report[mode] and SEQUENCES in report[mode], 'exact differential coverage ' + mode)
        # paired performance: B (accepted C TCP over the production Rust IPv4) versus the Rust TCP, same binary and CPU
        report['cpu'] = min(os.sched_getaffinity(0))
        raw = run(['taskset', '-c', str(report['cpu']), out / 'release-differential', '--bench'], 'performance.csv')
        rows = list(csv.DictReader(io.StringIO(raw))); labels = ['syn_synack', 'ack_establish', 'data_ack', 'send', 'receive_partial', 'retransmit']; report['performance'] = {}
        for n, label in enumerate(labels):
            r = [x for x in rows if int(x['workload']) == n]; c = statistics.median(float(x['C_ns']) for x in r); s = statistics.median(float(x['Rust_ns']) for x in r)
            report['performance'][label] = {'C_ns': c, 'Rust_ns': s, 'percent': 100 * (s / c - 1)}
        report['regressions_over_5_percent'] = sorted(k for k, v in report['performance'].items() if v['percent'] > 5)
        # checked Rust (overflow checks, panic=abort) under the sanitizer C objects
        common = [a.rustc, '--edition=2024', '-Dwarnings', '-Copt-level=2', '-Cdebuginfo=0', '-Coverflow-checks=yes', '-Ccodegen-units=1']
        def hostlib(net_rs, lib, log):
            root = lib.with_suffix('.rs')
            root.write_text('#![no_std]\n#[path=' + json.dumps(str(net_rs)) + '] mod net;\nunsafe extern "C" {fn abort()->!;}\n'
                            '#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
            run(common + ['--crate-type=staticlib', '-Cpanic=abort', root, '-o', lib], log)
        checked = out / 'libchecked.a'; hostlib(ROOT / 'arch/x86_64/rust/net.rs', checked, 'checked-rust-build.txt')
        run(cc + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', 'tests/host/tcp-differential.c', *objs['sanitizer'], checked, '-o', out / 'checked-differential'], 'checked-build.txt')
        report['checked_Rust'] = run([out / 'checked-differential'], 'checked.txt'); need(COVERAGE in report['checked_Rust'], 'checked complete coverage')
        # safe-core tests
        units_root = out / 'units-root.rs'
        units_root.write_text('pub mod net {\n' + ''.join('#[path=%s] pub mod %s;\n' % (json.dumps(str(ROOT / 'kernel/rust/net' / (m + '.rs'))), m) for m in ('checksum', 'context', 'ipv4', 'arp', 'tcp'))
                              + '}\n#[path=' + json.dumps(str(ROOT / 'tests/rust/tcp.rs')) + '] mod units;\n')
        run(common + ['--test', units_root, '-o', out / 'core-tests'], 'core-tests-build.txt')
        report['core_tests'] = run([out / 'core-tests', '--test-threads=1'], 'core-tests.txt'); need('test result: ok. 20 passed' in report['core_tests'], '20 safe-core tests')
        # named mutation controls: each mutant core, rebuilt as the host library, must make the release differential FAIL
        source = (ROOT / 'kernel/rust/net/tcp.rs').read_text()
        need(len({n for n, _, _ in MUTATIONS}) == len(MUTATIONS) >= 60, 'at least 60 uniquely named mutation controls')
        for name, old, new in MUTATIONS: need(source.count(old) == 1, 'unique named mutation ' + name)
        report['mutations'] = {}
        if a.skip_mutations: report['mutations'] = 'SKIPPED (development only)'
        else:
            for name, old, new in MUTATIONS:
                d = out / 'mutants' / name; (d / 'arch/x86_64/rust').mkdir(parents=True); (d / 'kernel/rust/net').mkdir(parents=True)
                shutil.copy(ROOT / 'arch/x86_64/rust/net.rs', d / 'arch/x86_64/rust/net.rs')
                for m in ('checksum', 'context', 'ipv4', 'arp', 'udp'): shutil.copy(ROOT / 'kernel/rust/net' / (m + '.rs'), d / 'kernel/rust/net' / (m + '.rs'))
                (d / 'kernel/rust/net/tcp.rs').write_text(source.replace(old, new))
                lib = d / 'libmutant.a'; hostlib(d / 'arch/x86_64/rust/net.rs', lib, 'mutant-%s-build.txt' % name)
                run(cc + ['-O2', 'tests/host/tcp-differential.c', *objs['release'], lib, '-o', d / 'differential'], 'mutant-%s-link.txt' % name)
                t = run([d / 'differential'], 'mutant-%s.txt' % name, check=False, timeout=600)
                exit_code = report['commands'][-1]['exit']
                need(exit_code != 0 and COVERAGE not in t, 'mutation control not detected: ' + name)
                report['mutations'][name] = {'detected': True, 'exit': exit_code, 'first_failure': (t.strip().splitlines() or [''])[-1][:200], 'mutant_sha256': sha(d / 'kernel/rust/net/tcp.rs')}
                shutil.rmtree(d / 'arch'); shutil.rmtree(d / 'kernel'); (d / 'differential').unlink(); lib.unlink()
        # entry frames: production archive versus the accepted C (release, kernel flags) on this host's gcc
        asm = run(['objdump', '-dr', '--no-show-raw-insn', archive], 'production-disassembly.txt')
        cref = run(['objdump', '-d', '--no-show-raw-insn', out / 'release-reference-tcp-B.o'], 'reference-tcp-disassembly.txt')
        frames = {n: {'C_reference': frame(cref, n + '_rB'), 'Rust': frame(asm, ENTRY[n])} for n in TCP}
        report['frames'] = frames; over = {n: v for n, v in frames.items() if v['Rust'] > v['C_reference']}
        report['frame_exceptions_pending_owner_approval'] = over
        need(all(n in FRAME_EXCEPTIONS and v['Rust'] <= FRAME_EXCEPTIONS[n] for n, v in over.items()), 'Rust frames not larger than accepted C except the disclosed list: %s' % over)
        funcs = dict(re.findall(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n\n|\Z)', asm, re.M | re.S))
        ours = {n: b for n, b in funcs.items() if n.startswith('vos_rust_net_tcp_') or '3net3tcp' in n}
        panics = sorted(n for n, b in ours.items() if re.search(r'R_X86_64_\w+\s+\S*(?:panic|unwrap_failed|expect_failed|bounds_check|slice_index|len_mismatch)', b))
        report['panic_free'] = {'functions': len(ours), 'with_panic_paths': panics}; need(len(ours) >= 13 and not panics, 'R3-N2 Rust TCP code has no panic path')
        net = {n: b for n, b in funcs.items() if n.startswith('vos_rust_net_') or '10vos_kernel3net' in n}
        external = sorted({t for b in net.values() for t in re.findall(r'R_X86_64_\w+\s+([A-Za-z_][\w.$]*?)(?:[-+]0x[0-9a-f]+)?$', b, re.M)
                           if not t.startswith(('.', '_R', 'vos_rust_'))})
        report['network_external_symbols'] = external; need(set(external) <= MEMORY_SYMBOLS, 'network code references only memcpy/memset/memcmp (no memmove): %s' % external)
        symbols = run(['nm', '--defined-only', archive], 'archive-owners.txt')
        for n in TCP: need(sum(x.endswith(' T ' + ENTRY[n]) for x in symbols.splitlines()) == 1, 'one Rust owner ' + ENTRY[n])
        csyms = run(['nm', out / 'release-driver-tcp.o'], 'C-symbols-tcp.txt')
        for n in TCP: need(' T ' + n in csyms and ' U ' + ENTRY[n] in csyms, 'C keeps the public ABI and forwards ' + n)
        driver = (ROOT / 'arch/x86_64/net_tcp.c').read_text()
        need(not re.search(r'\bmem(cpy|cmp|set|move)\b|\bfor\s*\(|\bwhile\s*\(|\bif\s*\(', driver), 'no C protocol logic remains in net_tcp.c')
        report['source_sha256'] = {n: sha(ROOT / n) for n in ['kernel/rust/net/tcp.rs', 'arch/x86_64/rust/net.rs', 'arch/x86_64/net_tcp.c', 'arch/x86_64/rust_kernel.h',
            'tests/host/tcp-differential.c', 'tests/rust/tcp.rs', 'tests/reference/net-tcp.c', 'tests/reference/net-headers/net_tcp.h', 'tests/reference/net-r3n2.json', 'scripts/test-tcp-rust.py']}
        report['status'] = 'PASS' if not a.skip_mutations else 'INCOMPLETE_MUTATIONS_SKIPPED'
    except Exception as e: report.update(status='FAIL', error=str(e)); raise
    finally:
        with (out / 'result.json').open('x') as f: json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'core_tests', 'mutations', 'release', 'sanitizer', 'checked_Rust')}, indent=2)[:6000])
    print('mutation controls detected:', len(report['mutations']) if isinstance(report['mutations'], dict) else report['mutations'])
if __name__ == '__main__': main()

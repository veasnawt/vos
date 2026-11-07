# R3-N1: Rust ARP/IPv4/UDP packet validation and protocol state

**Correctness, staged equivalence, resources, reproducibility, and the BIOS/UEFI network regressions all PASS. No benchmark regresses. Two stack-frame exceptions need explicit owner approval of this exact source: `net_ipv4_transport_checksum` uses 16 bytes against the C leaf's 0, and `net_ipv4_input` uses 1,704 bytes against the 1,672 that the CI runner's gcc-13 gives the C. No merge without owner approval.** This is the first R3 increment. Baseline is accepted main `879c51179ecb6bcff3d1f7ef52051947b858c059`, where R2 is formally closed.

## Scope and ABI

All 17 public functions of `net_arp.c`, `net_ipv4.c` and `net_udp.c` keep their C ABI, and their headers are unchanged. Each now forwards to a private `vos_rust_net_*` entry.

- **The Rust core (`kernel/rust/net/`):** five safe modules (`#![forbid(unsafe_code)]`). `checksum`, `context`, `ipv4`, `arp` and `udp` hold the checksums, Ethernet-II and local IPv4 validation, the envelope builder, ARP parsing and construction, the 4-entry neighbor table, ICMP echo state, and the 4-slot UDP endpoint.
- **The contexts:** exact `#[repr(C)]` mirrors with explicit padding (160/288/524/2400 bytes, layout asserted at compile time). A reset or copy produces the same bytes as the C `memset`/`memcpy`.
- **The FFI boundary (`arch/x86_64/rust/net.rs`):** it mirrors each C entry's null and validation order. Address inputs are copied first, as the C copies them. Generated frames are built in locals sized like the C locals (60/106/554/1514 bytes) and copied to the caller last, which preserves the "inputs may alias the output" contract.
  - `net_ipv4_begin` writes straight into the admitted output, as the C does.
  - `net_udp_input` hands non-UDP frames to the out-of-line IPv4 entry, as the C calls `net_ipv4_input`.
- **Stays C:** `virtio_net.c` (DMA rings and device ownership), `net_tcp.c` and the network boot glue are unchanged. No new network authority, connectivity, protocol behavior or hardware access is added.
- **Rust-only refusals, with no access or state change:** these cover cases the accepted contract leaves undefined:
  - a misaligned context
  - a frame, data range or received-datagram output overlapping the context a call mutates
  - a null checksum input, which returns `0xffff` so verification fails safe

  The C callers never produce any of them.
- **Copies at the boundary:** every copy is between disjoint objects, so the adapter uses `copy_nonoverlapping`, which lowers to `memcpy`. The kernel's `lib.c` provides `memcpy`, `memset` and `memcmp` but no `memmove`.
- **Two documented equivalences:**
  - `net_udp_receive` indexes `queue[head % 4]`. A valid context always has `head < 4`; the accepted C indexes `queue[head]` and would read out of bounds for a corrupted head.
  - Receive clears the copied slot after advancing the queue rather than before. With one owner, interrupts disabled and no callback in between, the resulting bytes are identical.

## Staged implementation

The migration landed in four commits. At each commit point the code was verified on the host:
- the Rust crate builds with kernel flags and warnings denied
- the three C files build with kernel flags and `-Werror`
- the **full** 635,285-comparison differential passes under ASan/UBSan
- all seven accepted host suites pass

| Stage | Rust-owned | Entries |
| --- | --- | --- |
| 1 | Internet and pseudoheader checksums | 2 |
| 2 | Ethernet-II and local IPv4 validation, IPv4 envelope | 5 |
| 3 | ARP parsing/construction, neighbor table, ICMP echo state (which resolves through ARP) | 12 |
| 4 | UDP validation, send, receive queue, close | 17 |

Intermediate commits carry a module-level `#![allow(dead_code)]` for items that later stages use. The final stage has none; it builds with warnings denied and has no dead code.

## Verification (accepted R2 development worker, campaign `r3-n1-net-dev-v1`)

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/net-{arp,ipv4,udp}.c` and their headers are byte-identical to `879c511`. All 17 public names are renamed `*_reference` at compile time only. |
| Differential | **635,285** comparisons plus **3,000** random 24-operation sequences, each in production-archive release, C ASan/UBSan and checked Rust. Every call runs on two identical 8 KiB arenas and must match on the return value and the *entire* arena: context, packet bytes and lengths, input immutability and aliasing. |
| Coverage | **Checksums:** every 0–2-byte input, plus random lengths to 1,500. **Validation:** every EtherType × destination/source class, every IPv4 header byte, total-length × frame-length grids, every fragment word, single-bit corruption at every offset, truncation at every length, every envelope length 0–1,481. **ARP/ICMP:** every MAC × IP first byte, every ARP field value, exact TTL edges (neighbor 5,998–6,001; query and ping 198–201) at normal and wrapped clocks, full-table replacement and RFC 5227 probes, every echo data length. **UDP:** every length field 0–1,500, every checksum value, every destination port, zero-checksum sends, queue exhaustion and wrap. Input/output aliasing throughout. |
| Differential sensitivity | 12 deliberate core mutations were built: 11 are detected. The 12th, `total > 1500`, cannot be distinguished: frames are at most 1,514 bytes, so the IPv4 total is at most 1,500 and the check is dead code in both C and Rust. |
| Safe-core tests | 51/51 |
| Named mutation controls | **78/78** caught by their designated tests, across checksums, unicast/MAC rules, output admission, Ethernet/IPv4 validation, envelope layout, neighbor expiry and replacement, query, probe, reply and solicitation rules, echo state and matching, ICMP validation, UDP layout/zero checksum/ports/lengths/queue/receive/close |
| Panic freedom | All 23 Rust network functions in the production archive have no panic, bounds or unwrap path |
| Host regressions | The 7 accepted suites (ARP 4,598, IPv4 1,806, UDP 136,730, TCP 3,808, TCP data 5,397, retry 1,711, close 8,505 checks) PASS under ASan/UBSan against the Rust core. TCP uses the Rust IPv4 envelope, view and checksums. |
| Native regressions | `test-arp`, `test-ipv4`, `test-udp` and `test-tcp` PASS on BIOS and UEFI with real wire traffic. TCP close/data/retry host suites and BIOS/UEFI boots PASS. virtio-net net (BIOS/UEFI) and NIC-absent boots PASS. The production normal boot, at 128 MiB and 2 GiB on BIOS and UEFI, PASSes. |
| Ownership | Each of the 17 entries has exactly one Rust `T` owner. The C objects define the 17 public symbols and reference only the Rust entries; `net_*.c` contain no protocol logic. |
| Reproducibility | Kernel `fa93d41d…` and archive byte-identical across independent rebuilds |

## Resources

- **Frames:** 16 of 17 Rust entries are at or below the accepted C frames. Examples: `ipv4_input` 1,704 = 1,704 (worker gcc-15), `udp_send` 680 < 696, `arp_query` 72 < 152, `ipv4_init` 8 < 184, `udp_receive` 8 = 8. **Exception:** `net_ipv4_transport_checksum` is 16 B against the C leaf's 0 B. LLVM keeps the inlined three-part sum in two callee-saved registers. The only zero-frame formulation would sum words into a running total instead of C's per-part folds; that equals the C for every length up to about 131 KB, but not bit-for-bit for every input, so the exact C arithmetic was kept. **Source-bound owner approval is required.**
- **Compiler-dependent exception:** the CI runner's gcc-13 compiles the frozen C `net_ipv4_input` with a 1,672-byte frame, against 1,704 from the worker's gcc-15. Both frames are dominated by the same 1,514-byte reply buffer. The Rust entry is 1,704 B. A register-return prototype did not shrink it. `test-net-rust.py` records this beside the checksum exception (at most 1,704 B), so the frame gate no longer depends on the host gcc. **Source-bound owner approval is required.**
- **Image:** loaded growth is **+0 bytes** (LOAD memsz 962,560 before and after). The ELF file is 28,208 B smaller.
- No heap, DMA, I/O or new persistent state.

## Performance (paired release medians, 9 samples, hosted, `performance.csv`)

| Workload | C ns | Rust ns | Change |
| --- | --- | --- | --- |
| checksum (packet payload) | 39.50 | 32.64 | −17.4% |
| UDP input queued | 204.06 | 146.21 | −28.3% |
| ICMP echo request answered | 388.76 | 218.16 | −43.9% |
| ARP request answered | 352.07 | 39.58 | −88.8% |
| UDP send | 395.29 | 199.77 | −49.5% |
| invalid UDP rejected | 54.77 | 34.15 | −37.7% |

These are host micro-benchmarks against the hosted C build, which calls libc `memcmp`/`memcpy`. They are not guest, NIC or throughput measurements.

## Worker network preflight and fixture findings

- **Containment:** inside the accepted worker the namespace has only `lo` and no IPv4 routes. TCP/UDP/IPv6 attempts to public, QEMU-gateway, NAT and LAN addresses all fail with `ENETUNREACH` (`worker-network-containment.json`). All 8 network fixtures attach the guest only through `-netdev stream` on an AF_UNIX socketpair, with `-nic none` and no IP sockets.
- **Pre-existing, not caused by R3-N1:**
  - `test-virtio-net.py`'s N4 compatibility section pins `vsh.elf` to the N4 evidence hash. `vsh` changed on 2026-10-25 (`4d3fc3c`), so that pin is stale on main. The script's network boots and host test are run; the stale identity check is not.
  - `test-tcp-close`, `-data` and `-retry` compare against historical N5 baseline directories that are not present. Their host suites and BIOS/UEFI boots are run; the historical benchmark pairing is not.
- **Changes to existing scripts:** the seven network scripts add the Rust core library (`scripts/net_rust_host.py`) to the host compile lines that now need it. No check is removed. Build smoke gains two steps: `test-net-rust.py` and `test-net-host.py`.
- **Failed iterations, preserved:** preflight run r1 stopped on the stale `vsh` pin above. Runs r2 and r3 passed.

## Exact-head CI history (PR #64)

Both failed runs are preserved on GitHub.

- **Run `42362596899` (`0bf2cfc`) failed the frame gate:** gcc-13 C `ipv4_input` is 1,672 B against Rust 1,704 B. Fixed by recording the compiler-dependent exception above (`f927186`).
- **Run `42364371196` (`f927186`) failed the USB.2-C3a panic-free link guard:** the adapter's `ptr::copy` lowered to `memmove`, which the kernel does not provide, so `compiler_builtins` was linked. The worker run had not executed that non-network step.
  - **Fix:** the copies became `copy_nonoverlapping`.
  - `net_udp_receive` now refuses an output that overlaps its context. With that refusal, its copy is between disjoint objects too. The differential's refusal section checks it, and removing the refusal makes the differential fail.
  - **Before pushing:** every hosted Build smoke step was run on the host. The two QEMU-booting checks, `test-usb2c3b2.py` and the QEMU runtime job, were left to CI.

## Evidence

`final-result.json`, `network-regressions.json`, `worker-network-containment.json`, `performance.csv`, `image-growth.json`, `reproducible-build.txt` and one representative BIOS UDP capture: `udp-bios-screen.png` and `udp-bios-wire.pcap`.

`retained-captures.json` indexes all 75 retained screenshots and wire captures by SHA-256; they remain in the evidence store. QEMU only; no physical-hardware claim.

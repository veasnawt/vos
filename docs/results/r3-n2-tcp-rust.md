# R3-N2: Rust TCP state machine

**Equivalence, mutation, panic-freedom, resource, reproducibility and the BIOS/UEFI network regressions all PASS. No benchmark regresses by more than 5%. Three entry-frame exceptions need explicit owner approval of this exact source: `net_tcp_send` 280 B, `net_tcp_retransmit` 248 B and `net_tcp_finish` 200 B against the accepted C. Whole-path stack depth is equal or lower for every public TCP function under GCC 13 and GCC 15. No merge without owner approval.** Baseline is accepted main `f8731c9688673bb1e9f0d738707f1ad2d32bcf4a` (R3-N1 merged).

## Scope and ABI

The accepted N5 passive TCP prototype moves from `net_tcp.c` into safe Rust. All 13 public functions keep their C ABI, and `net_tcp.h` is unchanged. Each now forwards to a private `vos_rust_net_tcp_*` entry.

- **The Rust core (`kernel/rust/net/tcp.rs`, `#![forbid(unsafe_code)]`):** handshake, payload, retransmission, closure (FIN-WAIT-1/2, CLOSING, TIME-WAIT, CLOSE-WAIT, LAST-ACK), RST rules, timers and abort. It uses the R3-N1 Rust IPv4 view, envelope and checksums directly. Non-TCP local frames go to the IPv4/ARP input, as the C calls `net_ipv4_input`.
- **The context:** an exact `#[repr(C)]` mirror of the 504-byte `struct net_tcp`. The three padding bytes after `closure_enabled` are an explicit field, so `net_tcp_close` writes the same bytes as the C `memset`, and field stores (including `net_tcp_abort`) never touch padding. Offsets are asserted at compile time.
- **Preserved exactly:** the ten state values, every return code, the 64-byte RX/TX bounds, the handshake (200), idle (400), TX (200), retry (50 × 3), close (400), TIME-WAIT (24,000) and hard (48,000) tick limits, packet formats, checksums, byte order, the serial-number tests at `0x80000000`, and the test-only ISN step (`+0x10000` per SYN).
- **The FFI boundary (`arch/x86_64/rust/net.rs`):** init addresses are copied before the context is borrowed, so they may alias it, as in the C. Segments are built in a local 118-byte buffer and copied to the caller last, so the output may alias the frame or the send data. Every response is prepared, and its output admitted, before state commits, as in the C. NOSPACE leaves the context and output untouched.
- **The buffer shift:** the C `shift` moves bytes down with a forward loop. Because the destination index is always below the source index, each read sees an original byte. The Rust version is the same in-place forward byte loop with bounds-checked access. It is not a `memmove` call; the production archive references only `memcpy`, `memset` and `memcmp`. For a corrupted context, where the C would run past the array, the Rust stops at the end of the 64-byte buffer. The exhaustive safe-core test covers every count and consumed value.
- **Rust-only refusals, with no access or state change:** these cover inputs outside the documented alias contract ("inputs/outputs must not alias the context"):
  - a misaligned context
  - a frame overlapping the context
  - send data overlapping the context
  - a receive output overlapping the context

  No caller produces any of them. This follows the R3-N1 policy.
- **Stays C:** `virtio_net.c` (DMA rings and device ownership), `net_boot.c` and the TCP boot-test sources (`net_tcp_*_boot.c`) are unchanged. No TLS, internet connectivity, outbound TCP, socket API, new TCP feature or hardware permission is added.

## Verification

Campaign `r3-n2-tcp-dev-v1` ran in the accepted development worker. Run r1 covered the candidate; runs r2 and r3 were CI equivalence and both stopped, as recorded below. Every hosted Build smoke step was also run on the host with GCC 13.

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/net-tcp.c` and `net-headers/net_tcp.h` are byte-identical to `f8731c9` (blobs `499664e4`/`e1048e16`, unchanged since N5). Manifest: `tests/reference/net-r3n2.json`. |
| Two oracle configurations | **A:** the frozen C TCP over the frozen R3-N1 C ARP/IPv4 (`879c511`). **B:** the frozen C TCP over the production Rust IPv4. Names are renamed at compile time only. |
| Differential | **374,029** three-way comparisons (A, B and Rust) plus **4,000** seeded random 40-operation sequences with fault injection. Each runs in the production-archive release build, the C ASan/UBSan build and the checked-Rust build. Every call runs on three identical 8 KiB arenas and must match on the return value and the *entire* arena: the 504-byte context, output bytes and lengths, NOSPACE/invalid-input immutability and aliasing. Every one of the ten states, and every return code the TCP API can produce, is required to occur. |
| Coverage | Init admission and address aliasing. Profile enable ordering. LISTEN: every flag byte, every bit flip of a SYN at every offset (with and without checksum repair), truncation, data offsets 0–15, IPv4 total lengths 0–80, ports, urgent pointer, SYN with data, and output admission and aliasing (NULL, 59/60/0 capacity, length inside the output or frame). SYN-RECEIVED: sequence/ACK offsets including ±1 and `0x80000000`, every flag byte, the 198–201 TTL boundary, RST, and peer port/IP/MAC mismatch. ESTABLISHED: RX fill 0/1/32/63/64 × TX outstanding 0/1/64 × sequence/ACK serial offsets × data lengths 0–65, window updates, partial receive capacities, send sizes and aliasing, and timer edges relative to the context's own deadlines. Retransmission: every tick 0–260, capacities, the retry budget, partial ACKs, zero window and tick wrap. Closure: every closing state, reached through the API with and without unread data, × flags × sequence/ACK offsets × lengths, duplicate FIN, FIN at the window edge, FIN retry budget, finish refusals, and TIME-WAIT/hard limits at 24,000/48,000 ±1. ISNs and peers near `0x7fffffff`, `0x80000000` and `0xffffffff`, and clocks near `UINT64_MAX`. |
| Safe-core tests | 20/20, including an exhaustive check of `shift` over every (count, consumed) pair |
| Named mutation controls | **84/84** rejected by the differential, across init/abort/close, profile gates, every timer, shift, segment layout and checksums, `emitted` bookkeeping, send/retransmit/receive rules, sequence and window tests, RST rules, every closing transition, header validation, peer identity, SYN-ACK and delegation. Five equivalent mutants were excluded after analysis, each listed in `scripts/test-tcp-rust.py` with the reason no API-reachable state distinguishes it. |
| Panic freedom | All 18 Rust TCP functions in the production archive have no panic, bounds or unwrap path. The R3-N1 gate (41 network functions) also passes. |
| Memory symbols | Network code references only `memcpy`, `memset` and `memcmp`; the kernel archive needs no `memmove` and no `compiler_builtins` member (USB.2-C3a gate PASS) |
| Ownership | Each of the 13 entries has exactly one Rust `T` owner. `net_tcp.o` defines the 13 public symbols and references only the Rust entries; `net_tcp.c` contains no protocol logic. |
| Host regressions | The 7 accepted suites PASS under ASan/UBSan against the Rust core: ARP 4,598, IPv4 1,806, UDP 136,730, TCP 3,808, TCP data 5,397, retry 1,711 and close 8,505 checks |
| Native regressions | `test-tcp`, `test-arp`, `test-ipv4` and `test-udp` PASS on BIOS and UEFI with real wire traffic. The TCP close/data/retry host suites and BIOS/UEFI boots PASS. virtio-net (BIOS/UEFI) and NIC-absent boots PASS. |
| Containment | Inside the worker the namespace has only `lo` and no IPv4 routes. Public, QEMU-gateway, NAT, LAN and IPv6 attempts all fail. All network fixtures use `-netdev stream` over an AF_UNIX socketpair with `-nic none`. |
| Reproducibility | Kernel `22a7aa27…` and archive `1052a93d…` are byte-identical across independent rebuilds |
| CI equivalence | **Host, GCC 13.4:** every hosted Build smoke step PASSes on the exact committed tree. **Worker:** the build, boot, R2, network (including the TCP and R3-N1 gates), USB.2 and QEMU containment-executor groups PASS (runs r2/r3). **Not run locally:** the QEMU runtime smoke job, and, in the worker, the USB.2-C4.1/C4.2a/USB.3-A/survey/lease steps; those passed on the host. Exact-head CI covers everything. |

## Resources

- **Entry frames:** these use the gate's corrected metric, which counts gcc's `add $-N,%rsp` as an allocation and ignores `sub $-N,%rsp` deallocations. Ten of 13 Rust entries are at or below the accepted C under both compilers. For example: `input` 328 against C 424/440, `init` 56 against 328, `ack` 184 against 216/200, `receive` 40 against 56.
- **Entry-frame exceptions, which need owner approval:**

  | Function | Rust | C (GCC 13.4) | C (GCC 15.2) |
  | --- | --- | --- | --- |
  | `net_tcp_send` | 280 | 232 | 232 |
  | `net_tcp_retransmit` | 248 | 216 | 232 |
  | `net_tcp_finish` | 200 | 24 | 24 |

  The Rust entries hold the 118-byte segment buffer that the C keeps in its callees. The C `finish` is 24 B only because its 216/232-byte `fin_packet` is a separate frame. Moving the buffer into the core would require writing the caller's output while the frame or context is borrowed, which is unsound when the output aliases the frame.
- **Whole-path stack depth**, from linked kernels with GOT-indirect calls resolved (`stack-depth.json`): every public TCP function is equal or shallower under both compilers.

  | Path | GCC 15 C | GCC 15 Rust | GCC 13 C | GCC 13 Rust |
  | --- | --- | --- | --- | --- |
  | `net_tcp_input` | 2,248 | 2,136 | 2,232 | 2,136 |
  | `net_tcp_send` | 480 | 464 | 480 | 464 |
  | `net_tcp_retransmit` | 720 | 512 | 688 | 512 |
  | `net_tcp_finish` | 512 | 464 | 496 | 464 |

  The TCP boot runners are 96–112 B shallower; `net_boot_run` is unchanged (7,944/7,960 B). The boot stack is 16,384 B. The deepest static path from `kernel_main` (11,944/11,920 B) is the pre-existing opt-in xHCI survey path and is unchanged by R3-N2. This is static analysis only.
- **Image:** the ELF file is 18,584 B smaller. Loaded memory grows by **8,192 B**: `.text` crosses two 4 KiB page boundaries after about 5.9 KB of net code growth, mostly inlining in the Rust entries. `.rodata`, `.data` and `.bss` are unchanged.
- No heap, DMA, I/O or new persistent state.

## Performance

Paired release medians, hosted, on the same CPU and binary (`performance.csv`, worker run r1, GCC 15). C is the accepted TCP (configuration B), with the same IPv4 core.

| Workload | C ns | Rust ns | Change |
| --- | --- | --- | --- |
| SYN → SYN-ACK | 257.6 | 140.9 | −45.3% |
| ACK → ESTABLISHED | 81.2 | 61.2 | −24.6% |
| data → ACK | 222.0 | 212.9 | −4.1% |
| send | 243.8 | 190.3 | −21.9% |
| partial receive | 105.7 | 107.9 | +2.1% |
| retransmit | 172.0 | 154.6 | −10.1% |

**Substitute for the absent N5 paired baselines**, authorized by the owner (`bench-pair.json`). The historical baseline directories `build/n5-tcp-data-20261024T185044Z`, `build/n5-tcp-data-20261024T185933Z/results` and `build/n5-tcp-retry-20261024T193224Z/results` are not present on this machine. In their place, the same N5 benchmark sources were built against the frozen accepted C TCP and against the Rust TCP, with alternating samples.

| Model | C median ns | Rust median ns | Change |
| --- | --- | --- | --- |
| payload handshake | 187.7 | 127.8 | −31.9% |
| retry | 84.4 | 81.0 | −4.0% |
| close | 196.1 | 184.4 | −6.0% |

One fix was made during development. An unguarded zero-length data copy in the segment builder made data→ACK about 12% slower; it was found by instruction and cycle counting. The C skips that copy (`if (n) memcpy`), and the Rust now does too. These are host micro-benchmarks, not guest, NIC or throughput measurements.

## Failed and superseded iterations

- **Worker run r2 STOP** (`JOB_FAILED:ci-usb2`): the linker died with SIGBUS while writing a USB.2-C3a.2 mutant test binary. The CI-equivalence harness deleted step outputs only at the end of each job, so the kernel/ISO set and several USB steps' trees filled the run's 256 MiB build tmpfs. This is a harness capacity fault, not a source failure. The repair swept each finished step's output (results harvested first). In run r3 the same group PASSed with the mount near 57 MB.
- **Worker run r3 STOP** (`JOB_FAILED:ci-usb-hosted`): `test-usb2c41.py` lists tracked files with `git ls-files`, and the sandbox deliberately does not expose the shared repository metadata. This is a sandbox limitation. The step PASSes on the host on the exact tree. The campaign's receipts budget then allowed no further run, so the remaining worker groups (C4.1–lease, QEMU runtime smoke) were not run in the worker.
- Both STOPs are sealed in the campaign receipts and preserved; neither is relabelled.
- Development-only harness errors, each fixed before any commit:
  - a misaligned `size_t*` in the differential, which UBSan reported in the C oracle;
  - a refusal check that mutated its own snapshot;
  - incomplete GCC 13 sanitizer runtime links on the host.

  No source defect was involved.
- Five candidate mutants were found equivalent and removed, with the reason recorded. Two mutants that did not compile were rewritten.
- A faster shift formulation was rejected because it reintroduced a bounds-check panic path.

## Evidence

The evidence files:
- `final-result.json`
- `network-regressions.json`
- `worker-network-containment.json`
- `performance.csv`
- `bench-pair.json`
- `frames.json`
- `stack-depth.json`
- `image-growth.json`
- `reproducible-build.txt`
- one representative BIOS TCP payload exchange: `tcp-data-bios-screen.png` and `tcp-data-bios-wire.pcap`

`retained-captures.json` indexes all retained screenshots and wire captures by SHA-256; they remain in the evidence store. Evidence is QEMU verification and static stack analysis only; there is no physical-hardware acceptance.

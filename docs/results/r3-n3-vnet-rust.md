# R3-N3: Rust virtio-net validation

**Equivalence, mutation, panic-freedom, resource, reproducibility, the full virtio-net compatibility suite and the BIOS/UEFI network regressions all PASS. No benchmark regresses by more than 5%. There is no stack-frame, whole-path or loaded-image growth, so no exception is requested. No merge without owner approval.** Baseline is accepted main `afe33b8671c4648cc946001013a4753a16fc52e2` (R3-N2 merged).

## Scope

The pure validation decisions of the accepted virtio-net driver move into safe Rust (`kernel/rust/net/vnet.rs`, `#![forbid(unsafe_code)]`), behind the existing network FFI adapter. The decisions are:
- feature choice (VERSION_1 and MAC required, no ACCESS_PLATFORM/IOMMU);
- queue size;
- the RX/TX queue count;
- the used-ring count bound, and each used entry's ID range, duplicate and length rules;
- the virtio-net header flags/GSO test;
- the notify-offset bounds;
- the send length, the wire minimum, and the receive capacity and output-overlap arithmetic.

`virtio_net.h` and the five public driver functions are unchanged. Every return value, error class, failure order and stop path is the accepted C behaviour.

**C keeps every device and memory authority:** PCI configuration and register windows, virtqueue and DMA memory, every volatile ring read, fences, initialization, reset and teardown, DMA allocation and mapping, notification, polling and ring publication, and thread/IRQ ownership and recovery. The Rust code receives only scalars that the unchanged C access sequence has already read. It references no symbol at all; it has no device, DMA, mapping or memory-routine reference.

- **Observation order:** `used_ok` keeps its C loop. It reads the count first, then, for each entry, `id` and then `len` through the volatile ring pointer. It asks Rust about each value pair, and stops at the first invalid entry exactly as before. No snapshot is taken, so no read is added, removed or reordered and no fence moves. The header test receives the two header bytes, which the C passes from the same non-volatile DMA-page reads.
- **Not migrated, with reasons:**
  - `buffer_ok` walks the kernel page tables and checks DMA-pool aliasing (address-space and DMA ownership).
  - The RX descriptor check depends on short-circuited volatile reads; passing all four fields would change the read count.
  - The MAC test is pure, but moving it grew the `vnet_init` frame by 16 B; it is not a listed candidate, so it stays in C.
- **No new capability:** no feature negotiation, descriptor format, queue size or device capability changes.

## Finding outside this scope (not changed)

`vnet_receive` re-reads the used entry's `id` and `len` from device-writable memory after `used_ok` has validated them, then indexes `net.pool[RX_BASE + id]` and copies `len - 12` bytes. A device that rewrites the entry between the two reads could make that index or length unvalidated. R3-N3 preserves the accepted observation order exactly, so the re-read is unchanged. Closing it, for example by validating and using one read, is a behavior change that needs separate review and authorization.

## Virtio-net regression baseline

`test-virtio-net.py` required `vsh.elf` to equal the N4 evidence hash `4a837277…`. Read-only rebuilds with the pinned toolchain establish the provenance:

| Commit | `vsh.elf` | `rixie.elf` |
| --- | --- | --- |
| `50cbc8b` (N4 evidence) | `4a837277…` | `77104da3…` |
| `4d3fc3c^` | `4a837277…` | `77104da3…` |
| `4d3fc3c` (2026-10-25, native application launching) | `e7af3ee5…` | `77104da3…` |
| `1b7da57` = main | `e7af3ee5…` | `77104da3…` |

The accepted app-handoff evidence (`docs/results/app-handoff/result.json`) records `e7af3ee5…`. `tests/reference/app-compat-baseline.json` records the historical and current identities with this provenance. The script now requires:
- every recorded hash to equal its cited evidence file;
- every superseded identity to be attributed;
- the rebuilt applications to equal the current accepted identities.

The N4 record itself is unchanged. The genuine `vsh` (BIOS) and `rixie` (UEFI) compatibility boots and the no-automatic-network-access check are kept, and they PASS.

## Verification

Campaign `r3-n3-vnet-dev-v1` ran in the accepted development worker. Every hosted Build smoke step was also run on the host with GCC 13.

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/virtio-net.c` and `net-headers/virtio_net.h` are byte-identical to `afe33b8` (blobs `85485705`/`772beeee`). Manifest: `tests/reference/net-r3n3.json`. |
| Transcript differential | The frozen driver and the production driver run against one PCI/DMA device model and write **byte-identical transcripts of 216,823 records**, in a release build linked with the actual kernel archive and an ASan/UBSan build with overflow-checked Rust. Coverage:<br>• all 1,024 combinations of ten feature bits plus 20,000 random words<br>• every queue size 0–65,536 plus large values<br>• every header byte pair<br>• the used-ring count from 0 to 12 and beyond, for RX and TX at six consumed indexes including 65,535<br>• fault injection at every entry: bad IDs 8/9/31/32/max, duplicates of the previous and first entry, and twelve length edges<br>• 60,000 random rings<br>• whole-driver scenarios: features, queue counts and sizes, QENABLE, notify offsets × multipliers at the 4,094–4,097 edges, MAC zero/multicast/unstable, absent device, reset hang, DMA budget, send lengths, NULL/DMA-pool/user buffers, TX completion ID/length/burst/stall/unsolicited faults, receive capacities and output overlap, RX length/ID/header/descriptor/count/duplicate faults, a 70,000-packet stream across the consumed-index wrap, and reset quarantine |
| Host suite | The accepted `tests/host/virtio-net.c` (real driver, PCI/DMA model, reset quarantine): 251 checks PASS against the Rust decisions |
| Named mutation controls | **26/26** change the transcript. Four equivalent mutants were excluded with reasons in `scripts/test-vnet-rust.py`: the used-count bounds are implied by the distinct-ID rule (pigeonhole), and a zero receive capacity is also refused by `buffer_ok`. |
| Panic freedom | All 11 Rust validation functions are panic-free. The R3-N1 and R3-N2 gates, now covering this module too, PASS. |
| Symbols | Network code references only `memcpy`, `memset` and `memcmp`, and the R3-N3 functions reference none. The kernel needs no `memmove` or `compiler_builtins` (USB.2-C3a PASS). Each of the 11 entries has one Rust owner, and `virtio_net.o` keeps the driver ABI. |
| Native regressions | Full `test-virtio-net.py`: net BIOS/UEFI, NIC-absent, and the `vsh`/`rixie` compatibility boots. `test-arp`, `test-ipv4`, `test-udp` and `test-tcp` on BIOS and UEFI. TCP data/retry/close host suites and BIOS/UEFI boots. The 7 network host suites. Real wire traffic is retained in PCAP, with screenshots. |
| Containment | Inside the worker, only `lo`, no routes and no reachability; every fixture uses AF_UNIX `-netdev stream` |
| Reproducibility | Kernel `ef5f7a02…` and archive `9ff8a399…` are byte-identical across rebuilds |
| CI equivalence | **Worker run r2 PASS:** every Build smoke group (build, boot, R2, network including the R3-N1/R3-N2/R3-N3 gates, USB.2 including C3a, QEMU containment executor, USB survey and lease) and the QEMU runtime smoke job. Each step's output is swept after it finishes; the build mount peaked near 60 MB. **Host, GCC 13.4, on the exact committed tree:** every hosted step PASS, including C4.1/C4.2a/USB.3-A, which need repository metadata the sandbox does not expose. |

## Resources

- **Frames:** every Rust decision is a 0-byte leaf. Every C driver entry frame (`vnet_init` 56, `vnet_send` 24, `vnet_receive` 72, `vnet_stop` 8) equals the frozen C under both GCC 13 and GCC 15.
- **Whole-path stack depth** (linked kernels, GOT-indirect calls resolved): equal or lower everywhere.

  | Path | GCC 15 | GCC 13 |
  | --- | --- | --- |
  | `vnet_init` | 312 = 312 | 304 = 304 |
  | `vnet_send` | 328 = 328 | 352 → 336 |
  | `vnet_receive` | 344 = 344 | 352 = 352 |
  | `net_boot_run` | 7,944 unchanged | 7,960 unchanged |
  | `kernel_main` | 11,944 unchanged | 11,920 unchanged |

- **Image:** loaded memory is unchanged (970,752 B; image end `0x1ED000`). The ELF file is 1,232 B larger.
- No heap, DMA, I/O or new persistent state.

## Performance

These are paired hosted medians with kernel-equivalent string codegen, alternating 10 runs on one CPU. The kernel is built `-ffreestanding -fno-builtin`, so `memcpy` is always a call. Host fortification would otherwise inline a bounded copy in only one of the two builds, which inflates the difference artificially (+30%).

| Workload | Change (worker, GCC 15) | Change (host, GCC 13) |
| --- | --- | --- |
| RX validate, copy and republish | +1.2% | −3.7% |
| TX send and completion | +2.5% | −0.3% |

These are host micro-benchmarks against a device model, not NIC throughput.

## Development iterations (disclosed)

- **MAC test:** migrating it grew `vnet_init` by 16 B, so it was returned to C.
- **R3-N2 gate:** the TCP gate's mutant trees copied a fixed module list. They now copy every other `kernel/rust/net` module, so all 84 TCP mutants still build; no check changed.
- **Harness fixes, before any commit:** a misaligned state hash index and missing notify-edge cases.
- **Git metadata:** one `git stash` used during development wrote temporary objects into the shared repository; they are booked in publication.

## Evidence

The evidence files:
- `final-result.json`
- `network-regressions.json`
- `worker-network-containment.json`
- `performance.csv`
- `stack-depth.json`
- `image-growth.json`
- `reproducible-build.txt`
- `vsh-provenance.txt`
- the BIOS virtio-net boot with real wire traffic: `net-bios-screen.png` and `net-bios-wire.pcap`
- the BIOS `vsh` compatibility boot: `vsh-bios-screen.png`

`retained-captures.json` indexes all retained screenshots and wire captures by SHA-256. Evidence is QEMU verification and static analysis only; there is no physical-hardware acceptance.

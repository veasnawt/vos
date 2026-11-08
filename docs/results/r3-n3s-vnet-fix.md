# R3-N3-S: virtio-net RX used-ring re-read fix

**The corrected driver reads each RX used entry's `id` and `len` once and uses those validated values. A deterministic hostile-device regression passes with no violation. The accepted driver at `f3683869` fails the same regression, which is the negative control. All five reintroduction mutants are detected. Honest-device behaviour is byte-identical to the accepted driver. The full virtio-net compatibility suite and the BIOS/UEFI network regressions pass. There is no stack-frame, whole-path, loaded-image or performance regression, so no exception is requested. No merge without owner approval.** The baseline is accepted main `f3683869901c22226f5472bbe737fcb58c798e5d` (R3-N3 merged).

## Issue record R3-N3-S-1

| Field | Record |
| --- | --- |
| Component | `arch/x86_64/virtio_net.c`, `vnet_receive()` |
| Introduced | `27fe770` (2026-10-24, the original bounded virtio-net adapter). Present in every main commit up to and including `f3683869`; R3-N3 preserved it deliberately and [recorded it](r3-n3-vnet-rust.md#finding-outside-this-scope-not-changed). |
| Class | Time-of-check/time-of-use double read of device-writable DMA memory (used-ring entry) |
| Trigger | A virtio-net device, or a hypervisor device model, that rewrites the head RX used entry after the driver has validated it and before the driver reads it again |
| Impact (frozen driver, demonstrated) | **Rewritten `id` ≥ 9:** `net.pool[RX_BASE + id]` is indexed out of bounds (UBSan: `index 11 … out of bounds for type 'uint64_t [11]'`). The driver then translates that unvalidated value with `phys_to_virt`, copies 12 header bytes from it, and indexes `q->desc[id]` out of bounds. In the model this is a wild access or abort.<br>**`id` 8:** selects a non-RX pool page and is refused only by the later descriptor check.<br>**Another valid `id`:** delivers the wrong buffer.<br>**Rewritten `len`:** either refused as EMSGSIZE, or delivers 1- to 13-byte frames below the validated 14-byte Ethernet minimum, and records an unvalidated length. The caller's capacity check still bounds the copy, so the length alone did not overflow the output buffer. |
| Fix | `ed738280` (this change) |
| Status | Fixed in the candidate; not merged |
| Limitation | This closes this double read only. See [Residual races](#residual-races). |

## The change

`used_ok()`'s loop becomes `used_validate()`. Its read sequence is unchanged:
- the count is read first and the fence is kept;
- for each entry, `id` and then `len` are read once through the volatile ring pointer, and the Rust decision is taken on that pair;
- it stops at the first invalid entry.

The function also returns the head entry's values exactly as read and validated, packed as `len << 3 | id`. The packing is exact: validated RX entries have `id < 8` and `26 <= len <= 1526`, and a static assertion pins it. It returns `-1` for invalid entries and `0` for an empty ring.

`vnet_receive()` consumes the packed values and never reads the used entry again. `used_ok()` remains a wrapper with its previous meaning, so TX completion is byte-for-byte the same code path.

The invalid and zero-count paths return before the result is used. DMA, PCI, MMIO, queue publication, reset and lifecycle code are unchanged, and no capability, feature or Rust code changed. The Rust archive is byte-identical to R3-N3 (`9ff8a399…`).

The packed return was chosen because it does not grow any frame:

| Shape | `vnet_receive` frame, GCC 15 | `vnet_receive` frame, GCC 13 |
| --- | --- | --- |
| `f3683869` (accepted) | 72 | 72 |
| Out-parameters | 72 | 88 (path 352 → 368) |
| Struct return | 88 | 88 |
| **Packed `int32_t` (adopted)** | **72** | **72** |

### Proof of no re-read

- **Source:** in the fixed `vnet_receive`, no expression reads `q->entries`. The only reads are in `used_validate`'s loop, one `id` and one `len` per entry.
- **Dynamic:** the hostile regression makes the RX ring page inaccessible during the call. It single-steps every access and counts the driver's reads of the head entry. In every one of the 440 cases the fixed driver reads `id` at most once and `len` at most once. The frozen driver reads both twice in 273 cases.
- **Mutation:** the gate rejects three variants that put back the second read (`reread-both`, `reread-id`, `reread-len`).

## Verification

Campaign `r3-n3s-vnet-fix-v1` ran in the accepted development worker, in two runs, with no repair runs. Every hosted Build smoke step was also run on the host with GCC 13.

| Gate | Evidence |
| --- | --- |
| Hostile device (`scripts/test-vnet-hostile.py`, new Build smoke step) | 440 cases, built with ASan/UBSan. RX ids 0–7 at consumed indexes 0, 5 and 65,535 (the wrap). After validation the device rewrites the entry to:<br>• ids 8, 9, 10, `0x10000`, `INT32_MAX` or `UINT32_MAX`<br>• another valid id<br>• lengths 0, 11, 25, 40, 1527, 5000, `INT32_MAX` or `UINT32_MAX`<br>• both id and length<br>Further cases: multiple completions, capacities 8 and 60, invalid initial id/length, duplicate ids, a bad header, the zero-count path and three TX rewrites.<br>**Fixed driver: 0 violations.** Every validated packet is delivered from the validated buffer with the validated length; invalid input is refused; the zero-count path reads nothing. |
| Negative control | `tests/reference/virtio-net-r3n3.c` is byte-identical to `f3683869` (blob `6d9b2da1`, manifest `tests/reference/net-r3n3s.json`). The same cases produce **160 memory violations (112 UBSan pool-index out of bounds), 273 second reads and 248 wrong results**. The gate fails if the frozen driver ever stops demonstrating the defect. |
| Mutation controls | Five mutants are all rejected: `reread-both`, `reread-id`, `reread-len`, `head-from-last-entry` and `head-len-dropped`. |
| Honest-device differential | The frozen and fixed drivers write **byte-identical 216,823-record transcripts** in release and ASan/UBSan builds (`6603e58b…`). The R3-N3 gate also still passes, with byte-identical transcripts against the `afe33b8` oracle and 26/26 mutants. |
| Host suite | 251 checks PASS |
| Native regressions (worker) | **Full `test-virtio-net.py`:** net BIOS/UEFI, NIC absent, `vsh` BIOS and `rixie` UEFI compatibility boots.<br>**On BIOS and UEFI:** `test-arp`, `test-ipv4`, `test-udp` and `test-tcp`, plus the TCP close/data/retry boots.<br>**Host:** the network host suites.<br>Real wire traffic is retained in PCAP, with screenshots. |
| Containment | Inside the worker: only `lo`, no routes and no reachability. Every fixture uses AF_UNIX `-netdev stream`. |
| Panic freedom, symbols | Rust is unchanged. The R3-N1/R3-N2/R3-N3 panic-freedom gates pass, and the kernel needs no `memmove` or `compiler_builtins`. |
| Reproducibility | Kernel `1c4f728c…` and archive `9ff8a399…` are byte-identical across rebuilds. |
| CI equivalence | **Worker run r2 PASS:** every Build smoke group (build, boot, R2, network including the R3-N1/R3-N2/R3-N3 gates and the new hostile gate, USB.2, QEMU containment executor, USB survey and lease) and the QEMU runtime smoke job. Each step's output is swept after it finishes. C4.1/C4.2a need repository metadata, which the sandbox does not expose, so they ran on the host. **Host, GCC 13.4, on the exact committed tree:** every hosted Build smoke step PASS (37/37), including the new hostile gate and C4.1/C4.2a/USB.3-A. |

## Resources

- **Frames and paths** (linked kernels, GOT-indirect calls resolved): all are equal to `f3683869`.

  | Path | GCC 15 | GCC 13 |
  | --- | --- | --- |
  | `vnet_init` | 312 = 312 | 304 = 304 |
  | `vnet_send` | 328 = 328 | 336 = 336 |
  | `vnet_receive` | 344 = 344 | 352 = 352 |
  | `vnet_stop` | 264 = 264 | 272 = 272 |
  | `net_boot_run` | 7,944 = 7,944 | 7,960 = 7,960 |
  | `kernel_main` | 11,944 = 11,944 | 11,920 = 11,920 |

- **Image:** loaded memory is unchanged (GCC 15: 970,752 B, end `0x1ED000`; GCC 13: 966,656 B, end `0x1EC000`). The ELF file grows by 608 B (GCC 15) or 576 B (GCC 13).
- **Performance:** paired medians of the frozen and fixed drivers, linked with the kernel archive, with kernel-equivalent string codegen. The fix removes one volatile read pair.

  | Workload | Worker, GCC 15 | Host, GCC 13 |
  | --- | --- | --- |
  | RX validate, copy and republish | −0.4% | −1.7% |
  | TX send and completion | −0.7% | −0.3% |

  These are host micro-benchmarks against a device model, not NIC throughput.

## Residual races

This change removes the one double read of the RX used entry. **It does not make the driver safe against an arbitrary malicious device.** Known remaining exposure:

- **No IOMMU.** `VIRTIO_F_ACCESS_PLATFORM` is refused, so the device has unrestricted DMA. A malicious device can already write kernel memory directly, and the hostile-device model mainly protects against buggy or confused devices and device models.
- **RX buffer contents** stay in device-writable DMA memory while the driver uses them:
  - `vnet_receive` copies the 12-byte header into `last_rx_header` and then tests the header bytes in place, so the recorded header can differ from the validated one;
  - the frame payload can change during the copy.

  Both affect diagnostics and data integrity, not driver memory safety: the copy is bounded by the validated length and the caller's capacity.
- **Descriptor checks** read the driver-written descriptor table, which also sits in DMA memory. The checks compare against expected values, and the values they read are not used afterwards.
- **Ring indexes and other entries:** the used index is read once into `count`. Entries after the head are validated, then re-read and validated again by the next call, before they are used.

Hardening these, for example by copying RX data into private memory before validation or by adding IOMMU support, would be a separate, separately authorized change.

## Evidence

Files in `r3-n3s-vnet-fix/`:
- `final-result.json`
- `hostile-regression.json`
- `hostile-fixed-transcript.txt` and `hostile-frozen-transcript.txt`: the per-case outcomes, including the frozen driver's UBSan reports
- `network-regressions.json`
- `worker-network-containment.json`
- `performance.csv`
- `stack-depth.json`
- `image-growth.json`
- `reproducible-build.txt`
- the BIOS virtio-net boot with real wire traffic: `net-bios-screen.png` and `net-bios-wire.pcap`. The UEFI boot's screen is byte-identical (deterministic output) and is indexed in `retained-captures.json`.

`retained-captures.json` indexes all retained screenshots and wire captures by SHA-256, and `SHA256SUMS` covers the directory. The evidence is QEMU verification, a host device model and static analysis only; there is no physical-hardware acceptance.

# Native networking: first N5 adapter increment

N5 starts from the accepted N4 merge `bbca69d850e8c168bfa824ace475a27226b2cdb7`.
Its tree matches accepted N4 publication `50cbc8bc49bd10d5272647b267ed23895d2f533d`.
Local main fast-forwarded to that merge; development uses `feat/n5-networking`.
The first increment sends and receives bounded raw Ethernet frames through a
modern virtio-net adapter in QEMU. It is published at `27fe770`. The subsequent
bounded Ethernet/ARP layer is implemented and verified below; IPv4 and ICMP
packet handling remain next.

## Existing infrastructure and choice

| Infrastructure | Existing behavior reused |
| --- | --- |
| PCI | Bounded bus-0 scan, configuration mechanism #1, firmware-assigned BARs |
| Virtio transport | Existing `vpci_parse_caps` and checked PCI configuration access window; no new MMIO mappings or changes to virtio-blk |
| Interrupts | Existing PIC/PIT, deferred device handlers and scheduler; this first driver polls and disables its INTx notifications |
| DMA/memory | PMM-owned 4 KiB frames below 1 GiB, kernel alias, supervisor mapping checks, W^X and no userspace DMA mappings |
| Execution | Kernel stays at IF=0; bounded TX polling yields periodically, demo RX waits sleep on the existing timer |
| Userspace authority | ABI v0–v3 and process-owned grants remain unchanged; there are no network syscalls or application network grants |

Modern virtio-net fits the same PCI access path as virtio-blk. The initial target
is QEMU bus 0: device `1af4:1041`, or transitional `1af4:1000` with subsystem 1
and a usable modern interface. The reused capability parser conservatively
requires at least eight device-configuration bytes; QEMU supplies this region.
No legacy transport, bridge traversal, BAR assignment or new interrupt controller
is needed. See the [VIRTIO 1.2 specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
particularly sections 2.7, 4.1 and 5.1.

## Bounded adapter interface

`arch/x86_64/virtio_net.h` exposes kernel-only initialization, send, nonblocking
receive and explicit stop. One kernel thread owns the adapter. It accepts only
VERSION_1 and MAC features; ACCESS_PLATFORM is refused because VOS has no IOMMU.
Offloads, merged receive buffers, packed rings, event indexes, multiqueue and
control queues are not negotiated.

- RX and TX each advertise eight split-ring entries. TX allows one request in
  flight; RX keeps eight private packet buffers posted.
- Eleven dedicated PMM frames (44 KiB) hold two rings, eight RX buffers and one
  TX buffer. The device receives only these DMA addresses. Caller memory is
  copied and never placed in a descriptor.
- Frames are bounded to 14–1514 bytes, without FCS. Short TX frames are padded
  to 60 bytes with zeros. RX buffers have a 12-byte virtio header and 1514 bytes
  of packet capacity. This increment supports untagged Ethernet with MTU 1500.
- Queue producer advances, descriptor IDs, duplicate completions, lengths,
  unsupported offload metadata and RX descriptor bounds are checked before copy
  or reuse. Publication and consumption use memory barriers.
- Empty RX returns EAGAIN. Insufficient caller capacity retains the packet and
  changes neither caller buffer nor length. Invalid mappings, user mappings,
  DMA aliases and overlapping output arguments are refused.
- TX has a fixed poll bound with periodic yields. The demonstration gives RX
  200 timer ticks per frame. Errors make the adapter unavailable and reset it.
- Reset must be confirmed before DMA pages are freed; PCI bus mastering is
  disabled first. An unconfirmed reset keeps the pages quarantined. No automatic
  reinitialization or retry of the frame exchange occurs.

As with virtio-blk, **there is no IOMMU**. Pool bounds constrain descriptors issued
by VOS; they do not contain a malicious physical device's DMA. Hardware verification
is limited to QEMU. Poll bounds do not establish hard real-time guarantees.

## QEMU nonmerged-header compatibility

The initial driver rejected a received `num_buffers=0`. The diagnostic run showed
that QEMU 10.2.1's socket-backed, nonmerged RX path leaves this field zero. VIRTIO
1.2 asks devices to supply 1 in this mode; the discrepancy is retained explicitly.
See [QEMU v10.2.1 receive code](https://github.com/qemu/qemu/blob/v10.2.1/hw/net/virtio-net.c)
and the preserved failed attempts in the results.

Because MRG_RXBUF is not negotiated, VOS always obtains a packet from exactly
one validated used descriptor. It ignores the header's buffer-count field and
never uses it to consume additional descriptors or enlarge a copy. Actual ring,
ID, packet-length and offload checks remain enforced. Host checks explicitly cover
zero, one and arbitrary ignored counts, as well as rejecting duplicate IDs,
out-of-range entries and oversized completions. No N3 or existing validator changed.

## Run the focused check

With the existing GRUB/OVMF tools and installed Rust target used by N4:

```sh
PYTHONDONTWRITEBYTECODE=1 RUSTC=/path/to/rustc \
  python3 -B scripts/test-virtio-net.py \
  --output "$PWD/build/n5-net-$(date -u +%Y%m%dT%H%M%SZ)"
```

Use a fresh output path and the established EFI build environment. The network
code is C; Rust is needed only to rebuild unchanged vsh/Rixie compatibility apps,
whose ELF hashes must match the accepted N4 identities. `make net-iso` and
`make net-uefi-iso` build the dedicated demonstration images under `build/n5-net`.
`NET_BUILD` must be nonempty. The demonstration requires its bounded test peer;
booting it without a responding peer correctly fails the receive deadline.

The driver uses a connected Unix socket pair with QEMU's
[stream network backend](https://www.qemu.org/docs/master/system/invocation.html).
The peer validates 16 exact guest Ethernet frames and returns 16 distinct reply
frames using EtherType `0x88b5`. Raw frame bytes, a PCAP, commands, serial logs and
registers are preserved. No TAP, NAT, LAN, internet connection or root privilege
is used. The host peer is test equipment; packet I/O inside the guest is VOS code,
not Linux networking inside the guest. No ping or IP connectivity is claimed.

## Permission direction and next work

The adapter runs only with the trusted `vos.net=1` boot option. A NIC present
during normal, vsh or Rixie boot is not automatically initialized. Applications
have no network entry point or network grant. The existing syscall table, Rust
SDK, process memory protection and console/file/channel capabilities are unchanged.

Before any userspace network API, access must be explicitly granted to a
process-owned object, with separate send/receive authority, bounded message and
queue resources, revocation and destination/protocol policy. Raw adapter access
must not become a default application right. This is a design direction, not a
claim that a userspace network capability API already exists.

## Bounded Ethernet and ARP increment

`net_arp.c` is a small kernel-owned protocol context over the unchanged adapter
packet interface. `net_ethernet_input` bounds untagged frames to 14–1514 bytes,
filters destination/source MACs and reads EtherType in network byte order. IEEE
802.3 length/LLC frames, VLAN, IPv4, IPv6 and other unsupported types are not
dispatched; the reserved type/length gap is invalid. There is no second adapter,
DMA pool, packet queue or userspace network interface.

ARP uses the Ethernet/IPv4 wire format in
[RFC 826](https://www.rfc-editor.org/rfc/rfc826.html): hardware/protocol types,
address lengths, operation and sender/target fields are checked before access.
Requests for our configured address produce a zero-padded unicast reply. An
outgoing broadcast query can resolve one neighbor at a time. Zero-sender-address
probes for our address receive a reply but never create a zero-address cache
entry; this is not full address-conflict detection or IP autoconfiguration.

Learning is deliberately conservative: accept valid local-target requests and
replies matching the active query, local target MAC/IP and Ethernet destination.
Ethernet source and ARP sender MAC must agree. Unrelated requests, unsolicited
replies, conflicting local-IP claims and invalid sender addresses do not populate
the cache. ARP remains unauthenticated: these checks do not prevent a local peer
from spoofing a plausible solicited response. No broader security claim is made.

The neighbor cache has four fixed entries with lazy expiry after 6000 PIT ticks
(about 60 seconds); a full cache replaces the oldest entry deterministically.
There is one pending query, expiring after 200 ticks. Cache hits emit no request;
there is no automatic retransmission, allocation or unbounded packet buffering.
Malformed, unsupported and unrelated frames generate no reply and do not alter
protocol state. Insufficient reply capacity changes neither state nor output.
Generated packets are bounded to 60 bytes; payload padding is zeroed.

The pure protocol context trusts kernel caller buffer lifetimes; it is not a
userspace ABI. Input and packet output may alias, while neither may overlap the
context and the output-length object must be separate. The adapter continues to
validate kernel mappings and use copied private DMA buffers. Timers are supplied
by the caller, so parsing requires no scheduler, allocator or new subsystem.

For focused checks, use the same EFI build environment as the first increment:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/test-arp.py \
  --output "$PWD/build/n5-arp-$(date -u +%Y%m%dT%H%M%SZ)"
```

The fresh-output test builds sanitizer-backed host checks and two bounded
BIOS/UEFI guests with an isolated Unix socket peer. It needs no Rust rebuild or
N3/N4 campaign. `make arp-iso` and `make arp-uefi-iso` build dedicated images under
`build/n5-arp`; ARP_BUILD must be nonempty. The demo uses 10.0.2.15 as an ARP
protocol address only. Its peer validates actual query/reply bytes and injects
malformed/unsupported frames. No IPv4 connectivity or ICMP ping is claimed.

The 3360 MiB ceiling is inherited N3 D2/C20 accounting, not an independent N5
allocation. Before this increment the owner explicitly approved **272 MiB of
additional ARP capacity within that unchanged ceiling**: two 128 MiB attempt
provisions and 16 MiB for evidence/metadata. A pinned ledger records all existing
N5 roots and source allocations. Positive growth, new roots and new files are
charged without deletion/shrink credits, with 2 MiB metadata reserve. The extra
meter is enforced alongside original physical reserves, local output bounds,
continuous isolation queries and the aggregate ceiling. All older N4/N5 usage
remains charged. See [ARP evidence](results/n5-arp.md) for exact final accounting.

Next: bounded IPv4 handling and ICMP echo, with explicit userspace permissions
required before exposing network access. Rixie Connected is not started. See
[adapter results](results/n5-first-network.md) and [Ethernet/ARP results](results/n5-arp.md).

# Native networking: virtio-net, ARP, IPv4, ICMP, UDP and TCP establishment

N5 starts from the accepted N4 merge `bbca69d850e8c168bfa824ace475a27226b2cdb7`.
Its tree matches accepted N4 publication `50cbc8bc49bd10d5272647b267ed23895d2f533d`.
Local main fast-forwarded to that merge; development uses `feat/n5-networking`.
The first increment sends and receives bounded raw Ethernet frames through a
modern virtio-net adapter in QEMU. It is published at `27fe770`. The subsequent
bounded Ethernet/ARP layer is published at `a6571c1`. Native IPv4/ICMP echo is
published at `33693b3`. Bounded kernel UDP is implemented and verified below;
UDP is published at `a5889f9`. A passive TCP establishment prototype is verified
below, published at `4e799c2`. Opt-in bounded payload exchange is verified below;
payload exchange is published at `ff251a7`. Bounded data retransmission is now
verified below; full TCP reliability and userspace authority remain future work.

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

The standalone raw-frame and ARP exercises above remain their original scope.
The IPv4 dispatcher below reuses shared Ethernet validation and existing ARP.
See [adapter results](results/n5-first-network.md), [Ethernet/ARP results](results/n5-arp.md)
and [IPv4/ICMP results](results/n5-ipv4.md). Rixie Connected is not started.

## Bounded native IPv4 and ICMP echo

`net_ipv4.c` reuses the existing Ethernet validator and the ARP context/cache.
It accepts only local unicast IPv4, checks version, IHL, available/total length,
TTL, header checksum, source/destination and fragmentation fields before reading
ICMP. Header options, fragments, forwarding and protocols other than ICMP are
rejected. Padding is excluded using IP total length. There is no allocation,
second driver, DMA pool or userspace entry point.

Echo requests/replies require code zero and a correct Internet checksum, including
odd payload lengths. Replies copy the request identifier, sequence and bounded
data, then generate fresh IP and ICMP checksums. At MTU 1500, an incoming echo can
carry up to 1472 data bytes. Generated Ethernet padding is zeroed. Kernel caller
lifetimes/alias contracts remain those of the ARP interface; output-length storage
must be separate from packet output, and buffers must not overlap the context.

Origination allows one pending echo, up to 64 payload bytes and 200 PIT ticks
for correlation. The caller must first resolve the peer through the same ARP
context. A miss emits nothing and queues no packet. An echo reply must match peer
IP/MAC, identifier, sequence, exact payload length and bytes within the deadline.
Wrong or unsolicited replies cannot complete it. No retransmission is automatic.
These checks provide correlation, not authentication against an on-link attacker.

The trusted demonstration selects `vos.ipv4=1`, local address 10.0.2.15 and a
directly attached isolated peer at 10.0.2.2. There is no subnet/router configuration,
gateway, DHCP, DNS, IPv6, UDP, TCP, fragment reassembly or ICMP error processing.
This is a real VOS ICMP exchange with an independent host wire peer; it does not
demonstrate internet access or the host's system ping utility.

With the same EFI tools and existing storage/isolation admission:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/test-ipv4.py \
  --output "$PWD/build/n5-ipv4-$(date -u +%Y%m%dT%H%M%SZ)"
```

`make ipv4-iso` / `make ipv4-uefi-iso` use a nonempty `IPV4_BUILD`.
The script supplies the bounded Unix-socket peer. An unconnected boot cannot
complete the demonstration. Each guest performs ARP resolution, sends two pings,
answers an incoming echo request and rejects injected invalid/unsupported or
unmatched traffic. Host sanitizers exercise bounds and maximum reply size; saved
PCAPs independently establish the actual wire bytes and checksums.

IPv4 continues the existing positive-growth 272 MiB ledger inside the unchanged
3360 MiB aggregate ceiling. Prior ARP usage and all failures stay charged; no
allocation is reset or expanded. Admission reserved a 128 MiB attempt bound plus
existing metadata/Git provisions; actual products and evidence are much smaller.
All original physical reserves and continuous isolation stop conditions apply.
Normal vsh/Rixie boots, syscall ABIs v0-v3 and application grants are unchanged.

## Bounded kernel UDP

`net_udp.c` owns one explicitly configured local endpoint and four FIFO slots,
each carrying at most 512 copied payload bytes plus source IP/port and checksum
presence. The same `net_ipv4` context supplies Ethernet/IP validation, ARP and
ICMP. Common IPv4 envelope generation and parsing were factored from the existing
ICMP implementation; the driver, DMA pool and accepted ARP rules are unchanged.
There is no socket syscall, application network capability or automatic NIC use.

The [RFC 768 wire format](https://www.rfc-editor.org/rfc/rfc768) supplies the
8-byte UDP header and IPv4 pseudoheader checksum. This bounded profile requires
UDP length to equal the IPv4 payload length, accepts no UDP options/trailer, and
rejects destination port zero. Source port zero is valid optional metadata;
the caller must not treat it as a reply endpoint. Local bind and remote send
ports must be nonzero; `uint16_t` types bound ports to 65535.

Every generated UDP datagram includes a checksum. A computed zero is emitted as
`0xffff`; received `0x0000` explicitly means no UDP checksum, which is permitted
for IPv4 and recorded in queue metadata. Nonzero checksums cover source/destination
IP, protocol 17, UDP length, ports and payload, including odd-length zero padding.
Checksum omission provides no payload integrity; no authentication is claimed.

Malformed, unsupported, unbound, oversized and full-queue UDP returns without
altering state or output. Queued data is independent of the RX buffer. Receive
copies into a complete caller-owned fixed-size datagram object and clears its
slot. An empty receive leaves that object untouched. Queue overflow discards the
new datagram and preserves FIFO contents; there is no allocation, blocking queue,
automatic retransmission or flow control. Closing erases the endpoint and queue.
One kernel owner runs at IF=0; this is not a concurrent socket API.

Sending requires an existing ARP neighbor; a miss emits/queues nothing. The
caller resolves it through the same ARP context before sending. Packet data is
staged before copying to caller output, preserving the existing alias/lifetime
contract. The unchanged adapter copies into private DMA memory. Adapter reset
must still be confirmed before its 11 frames are released; quarantine rules
remain in force. Protocol close does not substitute for adapter teardown.

The trusted `vos.udp=1` demonstration configures 10.0.2.15:4000 and an isolated
direct peer at 10.0.2.2:5000. With established EFI tools and storage/isolation
admission, reproduce the focused checks in a fresh directory:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/test-udp.py \
  --output "$PWD/build/n5-udp-$(date -u +%Y%m%dT%H%M%SZ)"
```

`make udp-iso` / `make udp-uefi-iso` require nonempty `UDP_BUILD`; the test script
provides the bounded Unix-socket wire peer. Both guests exchange real UDP bytes,
exercise checksum-zero cases, malformed lengths/checksums, a four-slot overflow,
FIFO recovery and explicit reset/teardown. Host checks sweep payload sizes 0-512
with ASan/UBSan and rerun the affected IPv4/ARP host regressions. vsh/Rixie sources,
ELFs and launch/ABI code remain unchanged; no unrelated N3/N4 campaign is repeated.

This increment has no routing, DHCP, DNS, TCP, IPv6, IP options/reassembly, UDP
options, unrestricted sockets or browser. The same 272 MiB positive-growth ledger,
3360 MiB aggregate ceiling, original reserves and continuous isolation guards
apply, carrying every earlier attempt. See [UDP results](results/n5-udp.md) for
raw PCAPs, manifest identities, authentic screenshots and final accounting.

## Bounded passive TCP establishment

`net_tcp.c` embeds the same IPv4/ARP/ICMP context and one fixed connection record
(host assertion: at most 512 bytes). It allocates no connection objects, payload
queues or heap memory. One kernel owner runs at IF=0. There is one explicitly
configured local port, with CLOSED, LISTEN, SYN_RECEIVED and ESTABLISHED states.
The demonstration opts in with `vos.tcp=1`; normal vsh/Rixie boots gain no NIC
initialization, syscall or network authority.

TCP header/sequence and checksum formats follow the relevant parts of
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293), without claiming full conformance.
Existing Ethernet/IP checks bound the segment before TCP reads it. Ports must be
nonzero; data offset must be in bounds, reserved bits zero and the mandatory
pseudoheader checksum valid. The existing UDP checksum calculation was factored
into a shared IPv4 transport helper without changing UDP zero-checksum semantics.
TCP has no checksum-omission mode: a zero checksum word passes only if the complete
checksum calculation validates it. Odd lengths are included in shared-helper tests.

This profile supports only a 20-byte TCP header and no payload. Options, data,
ECN/URG and FIN/RST processing are unsupported and safely rejected. A SYN to the
configured listening port produces one SYN-ACK with zero advertised receive
window. SYN consumes one sequence number, including wrap at 32 bits. The final
ACK must match stored IP/MAC/ports, peer sequence plus one and local ISN plus one.
Unsolicited ACKs, wrong tuples/sequences/ACKs, duplicate SYNs and a second
connection cannot establish or consume another record. Invalid input and short
output leave connection state and output unchanged. Packet input/output may alias
under the existing kernel caller contract; packet storage never aliases context.

A half-open connection expires after 200 PIT ticks. Its owner explicitly calls
`net_tcp_expire` to erase peer, sequence and deadline fields and return to LISTEN.
Even before that call, an expired ACK cannot establish. The caller supplies an
initial ISN; successive accepted SYNs advance a diagnostic counter by 65536.
This deterministic test generator is not the unpredictable production ISN design
required for a general TCP stack. Host checks cover sequence and timer wrap, stale
ACK rejection and reuse after expiry. Established state is released through
`net_tcp_close`, which erases the complete context. This is local abort/cleanup,
not a FIN or RST exchange with the peer. No retransmission is implemented.

The trusted demo first resolves 10.0.2.2 using the existing ARP cache, then listens
at 10.0.2.15:4001 for the controlled peer's port 5001. It deliberately expires one
half-open attempt, rejects invalid traffic and completes the next handshake with
a fresh diagnostic ISN. The host observes the serial timeout marker only to
coordinate the next SYN; all TCP packets themselves cross the actual virtio-net
wire interface and are retained in PCAP. No host or guest Linux TCP stack is used.

With established EFI tools and existing storage/isolation admission:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/test-tcp.py \
  --output "$PWD/build/n5-tcp-$(date -u +%Y%m%dT%H%M%SZ)"
```

`make tcp-iso` / `make tcp-uefi-iso` require nonempty `TCP_BUILD`; the test script
supplies the isolated Unix-socket peer. Host ASan/UBSan checks cover TCP and the
affected UDP/IPv4/ARP helpers. Two QEMU boots verify handshake, timeout/cleanup,
rejection counts, reset confirmation and all 11 DMA frames released. There is no
new DMA pool, userspace interface, routing, DNS, DHCP, TLS, IPv6 or browser.
The same 272 MiB continuation meter, aggregate 3360 MiB ceiling, reserves and
continuous isolation rules apply. See [TCP results](results/n5-tcp.md).

## Bounded TCP payload exchange

The trusted `vos.tcp-data=1` profile explicitly enables 64-byte RX and TX buffers
on the existing passive TCP state machine. `vos.tcp=1` retains the establishment
prototype's zero advertised window. There is one kernel-owned connection; the
complete context is 504 bytes, below the unchanged 512-byte bound. No ring-3
network API, application grant, heap allocation or additional DMA pool is added.

RX accepts only whole in-order segments fitting both actual free space and the
last advertised window. It copies bytes before advancing RCV.NXT and emitting
an ACK. Duplicate, overlapping, out-of-order and window-exceeding data produce
the current ACK/window without accepting or acknowledging new bytes. Partial
segment trimming and out-of-order reassembly are unsupported. Invalid checksums,
tuples or future/ambiguous ACKs cannot change buffers or sequence state.
Insufficient output capacity accepts nothing and returns an explicit error.

Kernel reads can consume a bounded prefix and erase the vacated bytes. Freed
capacity becomes remotely usable only when an explicit ACK or outgoing data
advertises it. Every advertised window is backed by available storage. Empty,
short/invalid output and buffer-full cases have explicit results; there is no
unbounded queue or hidden data overwrite.

TX copies one segment of 1-64 bytes and requires the peer's current window to fit
it. The copy remains until valid ACKs retire its prefix; partial ACKs retain and
erase the appropriate bytes. A second send is busy while bytes remain pending.
Stale ACKs are ignored without discarding valid incoming data or updating the
peer window; duplicate ACKs cannot refresh the TX deadline. Data with invalid
sequence placement cannot retire pending TX. ACK-only segments must fit the
receive window, including the zero-window boundary. Sequence/window comparisons
use bounded unsigned modular arithmetic, with wrap tested explicitly.

The owner explicitly expires unacknowledged TX after 200 PIT ticks and an idle
payload connection after 400 ticks. All connection buffers, peer/sequence/window
and deadline state are erased, returning to LISTEN with the endpoint mode intact.
This is an explicit local abort, not a successful delivery or FIN/RST exchange.
The demo reports the discarded pending RX/TX counts at timeout. Adapter reset
and DMA release remain separate and keep the original quarantine guarantees.

Reproduce with existing EFI tools and original isolation/storage admission:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/test-tcp-data.py \
  --output "$PWD/build/n5-tcp-data-$(date -u +%Y%m%dT%H%M%SZ)"
```

`TCP_DATA_BUILD` must be nonempty for `make tcp-data-iso` / `tcp-data-uefi-iso`.
The test supplies the isolated wire peer and records actual payload/ACK frames.
No unrelated milestone campaign is required. Full retransmission, reassembly,
FIN/RST closure, active open, congestion control and production ISNs remain
unsupported. See [payload evidence](results/n5-tcp-data.md).

### Relevant security and performance evidence

ASan/UBSan checks cover bounded buffers, malformed packets, exhaustion, timeouts,
ACK/sequence/window abuse and cleanup. The C boundary remains the existing
kernel-only copied-packet interface; DMA descriptors never point at callers'
buffers. vsh/Rixie, the SDK, capability grants and syscall ABIs remain unchanged.
No new security failure was observed in the focused checks; this is not a proof
of production security. Predictable diagnostic ISNs and missing full TCP recovery
remain material limitations. Before production TCP or cryptographic services,
VOS needs a properly seeded/reseeded kernel CSPRNG and secret-based per-connection
ISN generation, with failure behavior when secure entropy is unavailable. This
increment neither implements nor substitutes a deterministic generator for that.

Compact metrics identify source, compiler flags, host CPU/load and QEMU settings.
Sanitizer checks are correctness evidence; optimized hosted protocol-model
timings and unsanitized `-O2` QEMU scenario metrics are separate. Current records
include context size, private DMA, observed emulator CPU/high-water RSS, packet
timing and byte counts. Peer pacing, firmware boot and intentional timeout costs
are labelled; they are not maximum throughput or hard real-time claims. Historical
QEMU timing has a different prelude/load and is contextual only. The paired old/new
host model uses the same workload and compiler conditions. No optimization or
security-check bypass was introduced; simple bounded copies/shifts remain in use.

## Bounded TCP retransmission

The trusted `vos.tcp-retry=1` profile enables retransmission on the existing
payload connection. Other profiles keep their accepted behavior. Two byte-sized
mode/count fields and a 32-bit relative deadline use six previous padding bytes
between the ports and `since`; the context remains **504 bytes**, below the
unchanged 512-byte limit. No buffer, queue, heap allocation or DMA pool is added.

`net_tcp_retransmit` emits only retained, outstanding bytes starting at SND.UNA.
It does not advance SND.NXT or retire data. The retry is capped to the peer's
current window; a zero window produces no data or retry charge. Persist probes
are unsupported. Only a successful packet construction consumes one retry;
short output leaves state and the deadline unchanged.

The owner supplies monotonic PIT ticks. Fixed spacing is 50 ticks, at most three
retries, inside the unchanged absolute 200-tick TX horizon. The deadline is
relative to original `tx_since`; it cannot overflow because expiry is checked
before narrowing elapsed ticks. A late poll schedules the next attempt at least
50 ticks after the actual retry, without a catch-up burst. Valid partial ACKs
move the retained prefix and restart only the relative spacing, preserving the
original absolute horizon and retry count. Duplicate/stale/invalid ACKs do not
postpone retries. Full ACKs clear retry metadata; a new send starts a new budget.

Terminal expiry explicitly aborts and erases connection buffers/state. Local
abort and close suppress every later retry. The caller still confirms adapter
reset before freeing DMA frames. Incoming ACK-range, sequence and checksum checks
remain in place; ACKs cannot retire bytes beyond SND.NXT. Wraparound, shrinking
windows, partial/cumulative ACKs, retry exhaustion and destruction are host-tested.

With existing EFI tools and unchanged admission/isolation guards:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/test-tcp-retry.py \
  --output "$PWD/build/n5-tcp-retry-$(date -u +%Y%m%dT%H%M%SZ)"
```

`TCP_RETRY_BUILD` must be nonempty for `tcp-retry-iso` / `tcp-retry-uefi-iso`.
The isolated peer deliberately drops data, drops an ACK after delivery, deduplicates
a repeated segment, ACKs a prefix, and blackholes a final segment until retry
exhaustion. Its explicit loss/delivery decisions are recorded alongside raw PCAP;
partial prefix acceptance is a controlled peer action, not claimed IP fragmentation.
No historical QEMU campaign is repeated.

This is fixed, diagnostic data recovery, not production TCP: no RTT estimation,
exponential backoff, fast retransmit, persist probes, SYN/FIN/RST retransmission,
reassembly, wire closure or production ISN generator. Network access remains
kernel-only with ABI v0-v3 and capability boundaries unchanged. Optimized hosted
path comparisons use the accepted payload binary; unsanitized QEMU CPU/memory and
capture timings remain separate from ASan/UBSan correctness evidence. See
[retry results](results/n5-tcp-retry.md) for measurements and exact manifests.

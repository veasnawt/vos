# Rust USB boot-HID core (H1-B)

`kernel/rust/usb/hid/lib.rs` is an independent `no_std`, allocation-free Rust library with
`forbid(unsafe_code)`. Its parser and decoder are real reusable logic, not controller models.
It is **not linked into the kernel or any ISO in H1-B**: there is no live transport, request
submission, polling, PCI/MMIO, DMA publication, interrupt registration or physical-device activation.
The existing C ABI, PS/2 implementation and production boot behavior are unchanged.

## Supported subset

- USB 1.0/1.1/2.0 device descriptors; explicit low/full/high speed from the trusted transport.
  EP0 packet size is checked against speed. Device class is per-interface or standard IAD composite.
- Exactly one configuration, at most **4096 bytes**, **8 alt-zero interfaces**, **16 endpoints**
  across the configuration. Interface numbers are unique and within the declared interface count.
- At most one boot keyboard and one boot mouse. Each selected interface has exactly one interrupt-IN
  endpoint and one bounded HID descriptor. Endpoint addresses are unique across alt-zero interfaces.
  Packet limits, interval, directions, counts, descriptor lengths and IAD ranges are checked.
- Selected keyboard packets are at least 8 bytes; selected mouse packets at least 3; the selected
  endpoint maximum is 64 bytes (low speed 8). The actual supported reports remain bounded below.
  Unbound interfaces are structurally checked but never activated. Isochronous endpoints, alternate
  settings, multiple configurations, duplicate bindings and unknown descriptor kinds outside the
  supported class-specific subset are refused explicitly.
- Keyboard reports: exactly 8 bytes; modifiers plus six keys, release-before-press transitions,
  order-independent key sets and no duplicate text from repeated identical reports. Error/rollover
  usages 1-3, duplicate keys and malformed reports cause a reset barrier. Core events carry usages;
  character layout, typematic repeat and LEDs are not implemented here.
- Mouse reports: 3-8 bytes, bounded by the selected endpoint; first three boot bytes only. Signed
  X/Y and three button bits are decoded; extension bytes are ignored, not interpreted as wheels.
  Unsupported button bits are refused. Equal relative-motion reports are separate motions.

This is boot protocol, not an arbitrary HID report-descriptor interpreter. Report-descriptor
length metadata is bounded; the descriptor itself is neither fetched nor parsed.

## Transport contract

1. Validate complete copied descriptors with `Device::parse` and `Selection::parse`.
2. Construct one `Session` per selected interface with a nonzero device lifetime ID and generation.
   IDs are assigned by the trusted broker and must not be reused while an old request can survive.
   Keep one live session per device/generation/interface; copying a Binding grants no ownership.
3. `request_protocol` returns an opaque device/generation/interface-bound `ProtocolRequest`.
   `setup()` returns the eight bytes for `SET_PROTOCOL(0)`; **it sends nothing**.
4. Only the future trusted transport may call `confirm_protocol(request, Completion::Success)`,
   after the entire zero-data control transfer successfully completes on that exact device/interface.
   Stall, timeout or failure closes the generation. A request can be consumed only once.
5. Reports are refused before confirmation. The transport passes a copied slice from one completed
   interrupt-IN transfer, with its actual length; it must reject stale completions before this call.
6. `reset(newer_generation)` invalidates confirmation and pending requests; reconnect/reset requires
   another successful confirmation. `disconnect()` closes delivery and queues a reset barrier.

`Completion::Success` is a trusted-platform assertion, **not proof that hardware completed I/O**.
Opaque request fields prevent accidental cross-session/stale-request acceptance, not a malicious
kernel caller. No application can gain hardware or input authority through these Rust APIs.

## Input-broker contract

`Focus { owner, epoch }` is a broker-supplied identity, not a user capability. Call `focus` only after
existing VOS foreground/GUI ownership checks. Epochs strictly increase across handoff, revocation
and PID reuse; exhaustion fails closed. `take` requires the exact current owner and epoch and leaves
state untouched for a wrong owner. `revoke` drops authority and all pending input.

Each session has a **32-event** queue. A keyboard report is committed atomically (up to 20 transitions).
Overflow discards queued transitions, clears decoder state and produces an out-of-band `Event::Reset`
that cannot be crowded out. Malformed reports, ownership changes and disconnect/reset also produce
that barrier. The adapter must clear all downstream pressed/modifier/button state, cancel capture,
and use the existing GUI overflow/snapshot sequence barrier before consuming subsequent events.
After recovery, held input is ignored until a neutral report is observed; a release/neutral sample
is deliberately required before new presses can reach a new owner. `Reset` must never be ignored.

`Pointer { dx, dy, buttons }` uses **positive X right, positive Y down**. The existing GUI consumes
PS/2-oriented `pointer_packet.dy` and negates it. A future bridge must normalize exactly once:
either pass `-dy` through that unchanged seam, or introduce a reviewed normalized internal seam.
Keep existing GUI rights and primary-button semantics; decoding three bits does not expand the ABI.

The hosted adapters exercise these contracts against the unchanged console, editor, keyboard and
GUI implementations. Their C/Rust FFI glue, limited character mapping, injected completion success,
modeled scheduler/devices and byte-storage casts are **test-only**, not production integration or
native USB evidence. The unsafe FFI wrapper states its aligned/exclusive storage obligations.

## Resources and verification

No global state, heap, DMA pages or worker is acquired. Callers own all state, copied descriptors
and synchronization. On the verified x86_64 build: Session 264 B, Event 6 B; two sessions and the
4096-byte descriptor buffer total **4624 B**, below 8 KiB. Queue/event layout is a Rust-private detail,
not a C wire ABI. The target stack-frame ceiling is 512 B; measured LLVM frame sizes and limitations
are in the [results](results/h1b-rust-boot-hid.md).

```sh
RUSTC="$(rustup which --toolchain nightly-2026-09-03 rustc)"
python3 -I -B scripts/test-boot-hid.py --rustc "$RUSTC" --out build/h1b-hid-ci
```

Use a fresh output; the gate retains failure results and never retries automatically. Its default
runs core tests, real-C hosted adapters/PS/2 regression, eight assertion-detected mutants and
freestanding resource checks. The core is overflow-checked in hosted verification. ASan/UBSan
instrument C/harness code, **not Rust machine code**. No QEMU or hardware is operated by this gate.
Read [CONTRIBUTING](../CONTRIBUTING.md) and [SECURITY](../SECURITY.md); maintainer storage guards remain applicable.

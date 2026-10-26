# Pointer extension to GUI ABI v5

N6.3 keeps the published 24-byte create, 32-byte upload and 16-byte event
structures. Syscalls 16â€“20 and the keyboard-only GUI grant (15) retain their
existing behavior. Pointer authority is an additional trusted grant bit (16);
it is never inherited or installed from untrusted ELF metadata or shell text.

## Hardware and ownership

The single existing i8042 owner demultiplexes status bit 5 before decoding bytes.
Keyboard input still uses translated set 1. The auxiliary route resets the mouse,
requires ACK/BAT/standard ID 0, selects defaults and 1:1 scaling, then enables
reporting. Commands have finite polling bounds. IRQ1/IRQ12 stay disabled; the
existing scheduler-sleeping GUI input worker polls both ports. No new IRQ worker,
USB subsystem, device mapping or application network authority is introduced.

Only standard three-byte relative packets and the primary left button are
supported. Header synchronization, signed 9-bit deltas, overflow rejection,
controller errors and bounded partial-packet expiration are explicit. Loss
invalidates capture/button state and is reported through the event queue. A
fresh released packet is required before a press can establish capture.

Pointer initialization occurs when an explicitly authorized process creates its
surface. Failure returns EUNAVAIL and rolls back its frames. Surface release and
process revocation stop reporting, disable the auxiliary clock and clear the
input epoch. Service stop retains the existing bounded wake/join/reclamation
contract. Normal interactive sessions have no verification deadline.

## Events, focus and overflow

All events remain `{u32 type,u32 code,u64 sequence}`, 16 bytes, naturally aligned.
Types 1 (text key) and 2 (overflow count) are unchanged for keyboard-only owners.
Opt-in pointer owners additionally receive type 3 (motion) and type 4 (left-button
transition). Their code packs local x in bits 0â€“7, local y in bits 8â€“15, logical
left-down in bit 16 and inside-surface in bit 17. Bits 18â€“31 are zero.

The single surface is the sole hit-test target. Motion outside it is suppressed
unless an inside press established capture. A captured drag/release remains
routed to that owner outside the surface, with clamped local coordinates and
inside=0. Pressing outside and then moving inside while held does not synthesize
an inside press. Capture ends on release, loss, surface destruction or revocation.
There is no general focus manager, overlap policy or cross-process routing.

The bounded queue still retains older events and drops newest when full.
Keyboard-only sequence numbers index accepted keys exactly as before. Pointer
owners' sequence numbers also cover dropped events. Within each pump, queued
keyboard events precede decoded pointer packets; within a packet, movement
precedes a button transition. Each device stream remains ordered. No exact
cross-device physical-time ordering is promised.

Syscall **21**, `pointer_state(event*,16,0)`, requires pointer AND event authority,
a live surface and a writable user span. It returns 16 bytes with type 5 and the
latest decoded logical pointer state and sequence, or the existing negative
errors (EACCES, EINVAL, EFAULT). No address or hardware identifier is returned.
After type 2, the Rust demo takes this snapshot, restores its pressed visual,
and discards retained events whose sequence is <= the snapshot barrier. Thus a
dropped release cannot leave the demo permanently pressed. The snapshot covers
decoded input, not unread hardware bytes or a global wall-clock ordering.

The existing Rust `event()` remains key-only. New `pointer_event()`,
`pointer_state()` and Event accessors implement the opt-in extension. Other
languages can use the same C layout and int80 stub; no Rust-only ABI is created.

## Cursor and limits

The cursor is a temporary geometric 8x12 software arrow owned by the kernel.
Its fixed 384-byte underlay is saved/restored only through the supervisor
framebuffer alias. Position arithmetic widens to signed 64 bits, clips against
the validated framebuffer dimensions and respects pitch. Fill, glyph and surface
row updates hide/redraw intersecting cursor pixels, avoiding permanent trails.
No framebuffer, underlay or raw device buffer is exported to ring 3.

One foreground owner, one <=256x160 surface, 40 frames, 4096-byte uploads and
16 queued events remain unchanged. The driver has one pending mouse packet and
a 32-character keyboard staging queue; it stops draining rather than overwriting
either. Cursor shape and application pixels are independent of the event ABI.

The QEMU input sender uses actual relative-axis and button events delivered to
its emulated PS/2 device; see the [QEMU PS/2 source](https://github.com/qemu/qemu/blob/master/hw/input/ps2.c).
This establishes QEMU behavior, not physical-hardware compatibility. USB, wheels,
additional buttons, hotplug, multiple surfaces, window management and a
graphical Terminal are outside this contract.

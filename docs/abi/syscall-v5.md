# Native GUI interface: ABI v5, structures v1

This additive experimental interface uses the existing x86_64 System V register
contract and ring-3 int 0x80 entry. ABI v0-v4 syscall behavior remains unchanged.
Executables opt in with a VOS ABI note of 5; version 6 remains rejected.
A version number provides no authority.

The common trusted payload packages a separate Rust gui-demo ELF. Only the
explicit vos.apps=1 vos.desktop=1 vos.gui=1 boot route installs GUI authority.
vsh remains ABI v4 with its existing hello/Rixie whitelist; no GUI shell command,
arbitrary launcher, networking grant or automatic privilege inheritance is added.

## Authority and ownership

Trusted setup installs process/PID-bound GUI_CREATE, GUI_UPLOAD, GUI_PRESENT and
GUI_EVENTS rights. The initial implementation has one global foreground GUI
grant and one surface, at most one per process. It rejects a concurrent text
console owner or console lease. Applications cannot install or delegate grants.

Surface tokens are positive generation values, never pointers or Multiboot
indices. Lookups also require the owning live process, PID, thread and address
space. Recycled process pointers do not restore authority. Tokens are not reused.

The physical framebuffer remains supervisor-only. Pixel storage consists of
zeroed PMM frames accessible only to the kernel; no shared user/kernel mapping
or framebuffer pointer is returned. Source buffers always remain caller-owned.

## Calls

| Number | Operation | Arguments | Success |
|---|---|---|---|
| 16 | create | rdi=create pointer, rsi=24 | positive surface token |
| 17 | upload | rdi=token, rsi=upload pointer, rdx=32 | 0 |
| 18 | present | rdi=token, rsi=0, rdx=0 | 0 |
| 19 | release | rdi=token, rsi=0, rdx=0 | 0 |
| 20 | event | rdi=event pointer, rsi=16, rdx=flags | 16 |

Callers zero unused arguments. Present/release reject nonzero rsi/rdx.
Calls 16-20 return ENOSYS to ABI v0-v4. ABI v5 without the explicit grant returns
EACCES. Other existing calls retain their own grant/version rules; opting into
GUI does not bootstrap execution or text-console authority.

All structures use normal little-endian x86_64 C alignment:

- create: six u32 fields: version=1, size=24, width, height, stride, format=1.
- upload: six u32 fields: x, y, width, height, stride, length; then u64 pixels
  at offset 24. Total size 32.
- event: u32 type, u32 code, u64 sequence at offset 8. Total size 16.

## Surface bounds and presentation

Dimensions must be nonzero, width <=256 and height <=160. Create stride must
equal width*4. Format 1 is little-endian BGRX32: B, G, R, ignored X. The sink
masks X. The maximum backing allocation is 163,840 bytes / 40 frames, with
partial allocation rolled back on failure. A second live surface returns EAGAIN.

Uploads are rectangles wholly inside the owned surface. Stride must be a
multiple of four, at least width*4 and at most 4096. Length must equal
(height-1)*stride + width*4 and be <=4096. Arithmetic is widened and bounds are
checked with subtraction before coordinate addition. The descriptor and entire
input span are copied/validated before any pixel changes; errors do not partly
commit an upload.

Present synchronously copies the bounded kernel surface into a centered region
of the existing private console viewport. Applications cannot draw over desktop
chrome or select device addresses. No framebuffer reads, full-screen shadow,
GPU commands or asynchronous buffer ownership are introduced.

Release frees all surface frames and clears queued events; the process's input
grant remains until revocation/exit. Exit, fault, kill and cancellation revoke
GUI ownership and free retained frames. Invalid/stale tokens return EBADHANDLE.

## Keyboard events

The existing i8042 keyboard instance and decoder remain the only consumer.
Events initially carry decoded printable ASCII, Enter and Backspace, not raw
scan codes, key-up events or pointer events. Unsupported keys and Ctrl/Alt
combinations retain the existing decoder behavior.

Flag 0 waits; flag 1 is nonblocking and returns EAGAIN when empty. Other flags
return EINVAL. Blocking uses a wait queue and one scheduler-sleeping input worker,
not a spin loop or application deadline. Cancellation/revocation wakes a blocked
reader; the interrupted call returns EINTR, or process termination consumes it.

Type 1 is a text-key event; code is its byte value. A fixed 16-entry queue retains
older events and drops newest input when full. Type 2 reports a saturating loss
count and is returned before retained key events. sequence indexes accepted keys;
overflow carries the last assigned key sequence, so overflow-first delivery is
not a globally increasing delivery sequence. Bad output pointers retain events.

Focus acquisition and release use the existing keyboard initialization/suspend
drain boundary and clear software queues. Only the authorized foreground GUI
process receives events. No mouse events are synthesized.

## Rust API and build

The independent no_std vos_gui crate uses vos::Error and the existing runtime:

~~~rust
use vos_gui::{Surface, event};
let surface = Surface::create(256, 160)?;
surface.upload(0, 0, 1, 1, 4, &[0x80, 0x40, 0x20, 0])?;
surface.present()?;
let input = event(true)?;
surface.release()?;
~~~

Surface has a best-effort Drop release; kernel process teardown remains
authoritative on faults/cancellation. There is no allocator or UI/font/icon
framework in these bindings. C-compatible structures/stubs remain available.

After storage/isolation admission, use fresh nonempty output paths:

~~~sh
make ARCH=x86_64 USER_BUILD="$admitted_output/user" user-gui RUSTC="$rustc"
make gui-uefi-iso GUI_BUILD="$admitted_output/image" \
  GUI_APPS="$verified_common_elf_directory" RUSTC="$rustc" \
  GRUB_EFI_DIR="$verified_efi_module_directory"
~~~

GUI_APPS supplies verified read-only vsh, hello and rixie ELFs. Both kernel
sub-makes explicitly forward USER_BUILD. The normal image contains four
applications. GUI_VERIFY=1 with the dedicated verification configuration adds
the private probe; its query syscall is absent from normal builds.

## Failure semantics and limits

EINVAL: malformed descriptors, sizes, format, stride, rectangle, length or flags.
EFAULT: invalid input/output address span. EACCES: absent authority or wrong
process/context. ENOMEM: frame/service resource exhaustion. EUNAVAIL: unavailable
display/input. EBADHANDLE: missing/stale surface. EAGAIN: occupied surface/service
or empty nonblocking event queue. EINTR: a blocked wait lost authority.

This is a boot-selected, single-surface prototype, not a window manager or
graphical Terminal. No mouse, general focus manager, movable/resizable windows,
GUI application launcher, package installation or production hardware claim is
made. GUI apps do not currently launch from vsh.

The original initial-ELF-load worker retention defect was corrected by N6.2
bootstrap hardening: the outer route stops the service on every post-start
exit and joins the awakened worker through THREAD_FREE before returning.
The accepted [hardening evidence](../results/n6-gui/bootstrap-hardening/result.json)
records 18 actual rejected GUI loads per firmware with one worker created and
reclaimed per iteration, and unchanged process/frame/thread counts. This is
QEMU evidence; physical hardware and arbitrary service-restart scenarios are
not claimed.

N6.3 adds an explicitly granted pointer extension while retaining this ABI's
existing structures and keyboard-only behavior; see [pointer contract](pointer-v1.md).

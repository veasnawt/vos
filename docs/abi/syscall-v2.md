# VOS Native Application ABI v2: read-only files (PROPOSED)

| | |
|---|---|
| Status | **PROPOSED, 2026-10-20. Direction conditionally approved; internal Stage 1/2 foundations implemented. File ABI remains unimplemented.** |
| Base | `main` at `c5f5acb`; U1 and N3.4 complete; x86_64 only |
| Implementation plan | [N3.5 design, limits, failure model and test matrix](../plans/n3.5-file-api.md) |
| Existing contracts | [ABI v0](syscall-v0.md), [ABI v1](syscall-v1.md); existing numbers, errors and behavior remain unchanged |

This document specifies proposed behavior, not a claim about the current kernel. At the
base commit the loader accepts only ABI notes 0 and 1. On 2026-10-20 the overall direction
was conditionally approved. Later review authorized Stage 2 kernel-only nodes/handles;
[Stage 2 evidence](../results/n3.5-stage2.md) records actual tests. File syscalls and ABI
note 2 remain unimplemented and require Stage 3 review. No contract below changed in I2.

## 1. Versioning and compatibility

ABI v2 uses the existing ELF64 profile and VOS note with descriptor `2`. The proposed
loader and static checker accept exactly 0, 1 and 2; other descriptors remain rejected.
No change to entry registers, initial stack, address-space layout, `crt0.S`, floating-point
policy, `int $0x80`, register preservation, or return-frame validation is proposed.
As today, `rax` holds the number and signed result; arguments are `rdi, rsi, rdx, r10,
r8, r9`. All other registers, RFLAGS and the user stack are preserved across returning
calls, including calls that wait.

| Executable note | Calls 0–1 | Calls 2–5 | New calls 6–10 |
|---|---|---|---|
| 0 | Existing v0 behavior | `-VOS_ENOSYS` | `-VOS_ENOSYS`, before examining arguments |
| 1 | Existing v0 behavior | Existing v1 behavior | `-VOS_ENOSYS`, before examining arguments |
| 2 | Existing v0 behavior | Existing v1 behavior for channel handles; close also accepts file handles | Contracts below |

The v0/v1 specifications contain historical statements that no longer describe the entire
kernel (for example, v0's final description of preemption). Their existing runtime
contracts are interpreted together with v1, STATUS and current source. This proposal
does not rewrite those documents or change either ABI's behavior.

## 2. Proposed syscall table

Unused argument registers are ignored. `h` is an opaque positive signed 64-bit handle.
Lengths, offsets, cookies, buffer sizes and requested rights use the full unsigned
64-bit argument; high bits are validated, never silently truncated.

| No. | Name / proposed wrapper | Arguments in register order | Success |
|---|---|---|---|
| 0 | `exit` / `vos_exit` | status | Does not return; unchanged |
| 1 | `debug_write` / `vos_debug_write` | buffer, length | Length; unchanged |
| 2 | `yield` / `vos_yield` | — | 0; unchanged |
| 3 | `channel_send` / `vos_channel_send` | handle, buffer, length | 0; unchanged |
| 4 | `channel_recv` / `vos_channel_recv` | handle, buffer, capacity | Message length; unchanged |
| 5 | `handle_close` / `vos_handle_close` | handle | 0; channel semantics unchanged, v2 file close added |
| 6 | `file_open_at` / `vos_file_open_at` | directory handle, path pointer, path byte length, requested rights (`r10`) | New file/directory handle |
| 7 | `file_read_at` / `vos_file_read_at` | file handle, destination, length, unsigned byte offset (`r10`) | Byte count, 0 at EOF |
| 8 | `file_stat` / `vos_file_stat` | handle, destination, structure size | 0 |
| 9 | `directory_next` / `vos_directory_next` | directory handle, input cookie, destination, structure size (`r10`) | 1 entry, 0 at end |
| 10 | `file_initial` / `vos_file_initial` | trusted grant index (0 or 1) | Already installed handle |

Number 10 provides discovery without changing entry state or inventing `argv`, an auxiliary
vector, fixed magic handle values, or a new startup page. There are no write, mount, raw
device, open-by-inode, seek, duplicate, handle-transfer or root-discovery calls.

Proposed kernel constants are `SYS_FILE_OPEN_AT=6`, `SYS_FILE_READ_AT=7`,
`SYS_FILE_STAT=8`, `SYS_DIRECTORY_NEXT=9`, `SYS_FILE_INITIAL=10`; the new user file header
defines matching `VOS_SYS_*` constants. Pointer arguments need no alignment beyond being
valid user ranges; fixed record field offsets do not require aligned user destinations.

### 2.1 Reusing `SYS_HANDLE_CLOSE` (5)

The current channel validator accepts only `1..0xFFFFFF`. Proposed file tokens have bit 62
set, bit 63 clear, and a nonzero monotonically issued serial in bits 0–61. These domains
do not overlap. In v2, number 5 routes a token in the file domain to file close and other
values to the existing channel close. ABI v0 still gets ENOSYS; ABI v1 still runs the
existing channel close, including its existing invalid-handle results.

Do not generalize or re-encode the channel table. Calls 3/4 presented a file token still
get EBADHANDLE through their existing validation. File calls presented a channel token
get EBADHANDLE. Closing any valid file/directory handle, including a stale-volume handle,
succeeds without disk I/O; a repeated close gets EBADHANDLE. Close requires no rights.

## 3. Errors

Existing values **1–9 are frozen**: ENOSYS=1, EINVAL=2, EFAULT=3, EAGAIN=4, EPIPE=5,
EBADHANDLE=6, ENOMEM=7, EMSGSIZE=8, EACCES=9. Existing syscalls keep their present meanings
and validation order. The following values are proposed additions for file calls only.

| Name | Value | File-call meaning |
|---|---|---|
| `VOS_ENOENT` | 10 | Child name absent |
| `VOS_ENOTDIR` | 11 | Base/intermediate component is not a directory |
| `VOS_EISDIR` | 12 | Attempt to read directory bytes as a file |
| `VOS_ENAMETOOLONG` | 13 | Path >1024 bytes or component >255 bytes |
| `VOS_ELOOP` | 14 | Any symbolic link encountered, including the final component |
| `VOS_EIO` | 15 | I/O failure, corrupt metadata, or invalid device completion |
| `VOS_EUNAVAIL` | 16 | Configured volume/device unavailable without a known generation change |
| `VOS_ESTALE` | 17 | Volume binding invalidated by reset, removal, reinitialization or replacement |
| `VOS_ETIMEDOUT` | 18 | This call's operation deadline or device wait expired |
| `VOS_EINTR` | 19 | A nonfatal kernel interruption ended this file operation |
| `VOS_EUNSUPP` | 20 | Unsupported ext2 feature or non-file/non-directory target |

For new calls, ENOMEM means the per-process file-handle/session capacity or token space
is exhausted. EAGAIN means admission capacity is occupied, a previous abandoned request
of this process is not retired, or the bounded metadata/request/retry work budget ended.
It promises no readiness event and need not mean that an immediate retry will succeed.
Paths deeper than 64 components get EINVAL. No Linux errno numbers are imported.

## 4. Namespace, rights and names

Proposed rights are `VOS_FILE_READ=0x01`, `VOS_FILE_STAT=0x02`,
`VOS_FILE_ENUMERATE=0x04`, `VOS_FILE_TRAVERSE=0x08`; mask `0x0F`.
These bits belong to the separate file-handle table, not the channel rights namespace.

- All lookups use the calling process's own live file table and owner PID.
- `file_open_at` requires TRAVERSE on its base. Requested rights must be a subset of that
  handle's rights. Unknown bits get EINVAL; an attempted increase gets EACCES. Zero rights
  is allowed (a closable handle, usable for no other file operation).
- READ authorizes regular-file bytes; STAT authorizes metadata of the held object;
  ENUMERATE authorizes child names/types; TRAVERSE authorizes child lookup/open.
- Directory READ is an inheritable ceiling for opening readable children, not permission
  to read raw directory blocks. Unused rights on a node type grant no operation on it.
- `open_at` does not add rights on discovering a child. For example, opening an intermediate
  directory with only TRAVERSE intentionally loses the ability to derive READ handles.
- The kernel grants up to two initial non-root directories from explicit trusted startup
  configuration before `proc_start`. A program cannot select a device, grant path, inode,
  identity or rights in that configuration. A grant applies to a selected process launch,
  not every executable with a matching user-supplied display name.
- Ext2 owner, group and mode are descriptive policy inputs. They grant no ambient authority;
  uid 0 and executable mode bits provide no bypass. No user identity or POSIX ACL system
  is introduced. Trusted configuration must choose the appropriate directory subtree.
- No initial grant may name volume-root inode 2. No global root, current working directory,
  `AT_FDCWD`, implicit root handle, path-to-device mapping or fallback grant exists.

The proposed naming decisions (D-S5/D-S6) are **case-sensitive exact byte lookup and no
Unicode normalization**. NFC and NFD spellings, case variants and invalid UTF-8 byte
sequences remain distinct. N3.5 accepts opaque name bytes supported by ext2, not a Unicode
text API. This narrows the older architecture proposal's UTF-8 suggestion without adding
validation/folding tables. Display software must escape unsafe/non-text bytes. Nothing
normalizes, case-folds, percent-decodes, or treats a Unicode lookalike as `/` or `.`.

Path syntax:

- An explicit 1–1024-byte relative path; the length excludes a terminator. The kernel
  copies exactly that many bytes once, appends a private NUL, and rejects embedded NUL.
- `/` is the only separator; leading/trailing `/` and empty components are EINVAL.
  Each component is 1–255 bytes; at most 64 components, including `.`.
- `.` is a lexical self step. **Every `..` component is rejected with EACCES**, including
  `a/../b`; this deliberately stricter rule avoids trusting on-disk parent links.
- Every path is fully checked lexically before disk lookup. Every lookup uses
  `ext2_lookup` for one non-dot component plus checked `ext2_stat`; do not expose
  `ext2_resolve`, which accepts absolute paths and honors `..`.
- Symlinks in any position get ELOOP. No readlink or follow flag is provided. FIFO, socket
  and device targets get EUNSUPP at any component; a regular-file intermediate component
  gets ENOTDIR. Entries of special kinds may be enumerated.
- Reaching inode 2 through an ordinary child entry is refused with EACCES as well.

Authority is reachability through named child entries of a trusted provisioned namespace.
Hard links to regular files are aliases to the same data; they do not add an inode-opening
API. This is not authentication of a hostile disk's directory graph or contents: see the
explicit trust boundary and risks in the implementation plan. Bounds stop directory cycles
from creating an unbounded single call; they do not prove a global tree is acyclic.

## 5. Exact call contracts

For calls 6–9, the first failing check determines the result in this order unless a
call-specific step says otherwise:

1. ABI gate; own live file handle; object kind needed by the operation; required right.
2. Scalar sizes, flags/rights and arithmetic; then pointer validation and lexical path
   validation (for open: copy the whole path before parsing it).
3. Reserve any new handle slot; obtain a bounded operation ticket. No disk work on failure.
4. Check the volume binding and execute bounded operations; after every wait check kill,
   deadline and binding again. During I/O, kill wins over user return; a known local timeout
   is reported as ETIMEDOUT, then a binding change as ESTALE, then the ordinary layer error.
5. Recheck kill and volume before committing a result. Copy out once, or install the new
   handle atomically. Release private work state on every path.

Missing STAT or READ is not inferred from other rights. Pointer errors do not consume a
directory entry or install a handle. With multiple invalid scalar fields their checks follow
the order of the argument list; for open, total length, rights mask/subset, copied path syntax,
component lengths and depth are checked in that order (syntax is scanned left to right).
Nonzero read checks length and offset arithmetic before its destination.

### `file_open_at(base, path, path_len, rights)`

The base must be a directory and have TRAVERSE. Copy/validate the path and requested rights
as above. Reserve one of the process's eight file slots, then walk the copied components.
The final object must be a regular file or directory. Each intermediate object must be a
directory. On success install one handle with exactly the requested rights and the current
volume binding; return the token. No output pointer, mutable file position, global pathname
or open-node cache is involved. Any failure rolls back the reservation and references.
Opening `.` makes an independent handle with equal or reduced rights. Closing the base
later does not revoke already derived handles.

### `file_read_at(h, dst, len, offset)`

The handle must name a regular file and have READ; a directory gets EISDIR before the READ
check. `len > 4096` or unsigned `offset + len` overflow gets EINVAL. After those checks,
`len == 0` returns 0 without inspecting dst or the device binding, even for a stale volume.

For nonzero lengths validate **the full requested destination range**, including when
offset is at/past EOF. Every page must be user-accessible and writable at every level.
Read at most 4096 bytes into the process's private kernel bounce buffer. A successful
read returns `min(len, max(size-offset, 0))`; sparse holes are zero bytes. A successful
short read means EOF. Offset is explicit: no shared or per-handle seek state changes.

An ext2 error after a private prefix has been read discards that prefix and returns an
error; **no bytes are copied to user space on an error**, including timeout, EINTR, stale
binding and EFAULT. This deliberate all-or-error behavior differs from POSIX partial
success on an I/O error. A caller may retry the same offset. It is possible to implement
because one entire bounded call fits in the bounce buffer. EOF and short reads are still
ordinary nonnegative results. Bytes beyond a successful returned count are untouched.

### `file_stat(h, dst, size)`

Requires STAT on a file or directory. `size` must equal 64 (otherwise EINVAL), and the whole
destination must be writable user memory. Read validated metadata and return the structure
below in one copy, with reserved bytes zero. The result is 0. An error writes nothing.

| Byte offset | Type | Meaning |
|---|---|---|
| 0 | u64 | Size in bytes (directory size is descriptive, not raw-read authority) |
| 8 | u64 | Allocated 512-byte blocks, as validated/described by ext2 |
| 16 | u32 | Kind: regular=1, directory=2 |
| 20 | u32 | Ext2 mode (including type/permission bits), widened and zero-extended |
| 24 | u32 | UID |
| 28 | u32 | GID |
| 32 | u32 | Link count |
| 36 | u32 | This handle's rights |
| 40 | u8[24] | Reserved, all zero |

This is a new fixed 64-byte little-endian ABI layout, not a copy of `struct ext2_stat`.
No padding, pointers, raw inode numbers, device IDs or kernel addresses are exported.

### `directory_next(h, cookie, dst, size)`

Requires a directory and ENUMERATE. `size` must equal 272; validate the entire destination
even on EOF. Cookie 0 starts enumeration. Other cookies are byte positions: they must be
at a validated record boundary in this same directory, or exactly its size for EOF;
values beyond size get EINVAL. Cookies are not authority and may be guessed: they can
only select another position in the held directory. They are not valid across volume
bindings and have no meaning as inode numbers or addresses.

Use a private cookie while scanning with `ext2_readdir`. Omit `.` and `..`; skip unused
records through the existing parser. Validate the selected entry's inode/type with
`ext2_stat`, including images without the filetype feature, then copy exactly one record:

| Byte offset | Type | Meaning |
|---|---|---|
| 0 | u64 | Cookie to pass for the next call |
| 8 | u32 | Kind: unknown=0, regular=1, directory=2, character=3, block=4, FIFO=5, socket=6, symlink=7 |
| 12 | u32 | Name length, 1–255 |
| 16 | u8[256] | Exact name bytes followed by NUL; unused bytes zero |

Return 1 with one 272-byte output; return 0 at end **without changing dst**. On error return
a negative code without changing dst. There is no hidden enumeration cursor, so retry,
rewind (0) and two independent enumerators are straightforward. Entry order is on-disk
order, not sorted; no pagination allocation or multi-record copy is needed. Enumeration
does not create a handle or confer permission to open/read any entry.

### `file_initial(index)`

ABI gate first, then index: only 0 and 1 are defined (other values EINVAL). Return the live
token already provisioned in that trusted grant slot, or EBADHANDLE if no grant was
installed or it has been closed. Repeated discovery returns the same token, allocates
nothing, and never recreates a closed grant. It performs no disk I/O and can return a
live token whose volume is stale; the next actual operation reports ESTALE.

## 6. Handle lifetime and user-copy rules

File tokens are globally unique within a boot, allocated from a monotonically increasing
62-bit serial without wrap or reuse. Exhaustion fails closed with ENOMEM. They are neither
random nor secrets. A token is accepted only if it exactly matches a live entry of the
calling process and that entry's owner pointer and non-reused PID match. A token belonging
to another process, a closed token, wrong-domain values, 0 and negative values all get
EBADHANDLE. Guessing the exact value of a handle the caller already owns cannot be
distinguished from using that handle and adds no authority. Unlike files, old channel
tokens retain their documented 16-bit generation wrap and process-local numeric aliasing.

File slots are separate from the existing eight channel slots: up to eight files/directories
including initial grants, plus the existing eight channels. No handle transfer is added.
One node descriptor lives directly in each file slot; two opens of the same file have
independent rights and lifetime. Closing one does not affect another.

Copy path input with `copy_from_user`; stage outputs in private kernel memory and copy
with `copy_to_user`. Extract a narrow writable-range validation helper from the existing
user-copy checks to reject invalid output before disk I/O, without writing dummy bytes.
Copy-out repeats the checks after any wait. Reject overflow, noncanonical/kernel/null/
guard/supervisor pages, and nonwritable destinations; do not dereference user pointers in
ext2 or DMA. Full-range checks precede the first byte copied. No page-table pinning is
needed under the existing one-thread-per-process, one-CPU, no-shared-memory model: process
pages stay alive until its thread has unwound, exited and been reaped.

The syscall checks pending kill before copying/installing and after every wait. A killed
process unwinds its file call and never returns to user mode. Close all files after that
unwind, alongside existing channel cleanup, on exit/fault/kill and failed startup/destroy.
Driver-owned abandoned requests retain only driver buffers and scalar ticket identity,
never a process pointer, user destination, file-slot pointer or bounce-buffer pointer.
Detailed retirement, deadlines, volume invalidation and teardown gates are in the plan.

## 7. Conditional-review clarifications (2026-10-20)

- **Encoding proof:** every legacy channel token is <=`0xFFFFFF`; every file token is
  >=`0x4000000000000001` and <=`0x7FFFFFFFFFFFFFFF`. The sets are disjoint. Close dispatch
  examines the full 64-bit value; never mask/truncate to a channel slot. V1 always uses
  its original validator. A file serial never wraps; exhaust before reuse, including
  process-slot reuse. Test the last two serials, exhaustion, closed-token retries and
  legacy channel-generation rollover without strengthening/changing the v1 contract.
- **Paths:** validate the complete copied byte string before any filesystem walk. Reject
  leading `/` and every exact `..` component; do not normalize then validate. Check inode
  type after every lookup, including the final component, and refuse every symlink.
- **No pending user pointers:** the future syscall continuation retains the architectural
  trap frame (which necessarily contains the user's registers), not a usable user-memory
  pointer in a scope, driver slot, session buffer or queued request. After waits, decode
  the output address again from that live frame and revalidate the complete writable
  range before the single copy. Never hand that address to storage. Keep the frame,
  process pages and private kernel staging alive until unwind; no asynchronous copy-out.
- **Read edge cases:** owned regular handle + READ is checked even for length zero. Zero
  returns 0 without checking destination or volume. Nonzero reads validate the full
  requested writable range even at EOF. Length >4096 or unsigned offset+length overflow
  is EINVAL; invalid ranges/permissions are EFAULT. Every negative read result leaves
  all user bytes untouched, even if ext2 filled a private prefix before failing.
- **Cookies:** an exact boundary is a valid position, not proof of provenance. A forged
  boundary may enumerate only the caller's held directory; a mid-record or oversized
  position fails. Work on a private cookie, advance only in the returned record after
  complete validation/copy; EOF and errors leave output unchanged. A stale binding fails
  before applying any cookie to replacement media. Corrupt records fail without output.
- **Output initialization:** zero the entire 64/272-byte staging record first, then fill
  fixed fields and exact name bytes. Validate inode/type and length; include a NUL and
  zero the unused name tail and reserved fields. Do not copy native ext2 structures.
- **Tick-independent termination:** 200 ticks is not a wall-clock bound with IF=0. The
  metadata/backend/retry budgets and a hard maximum of 50,000,000 polls/request remain
  independent of tick progress. No scoped fallback may restart that poll budget. Scope
  deadlines can shorten it; they cannot replace it. Live/synthetic tests must freeze
  tick progress and assert the finite-work termination directly.

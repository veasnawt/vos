# ABI v4: bounded native execution

ABI v4 extends the existing x86_64 int 0x80 convention. Syscalls 0–13 and
ABI v0–v3 behavior are unchanged. Numbers 14 and 15 return ENOSYS for
applications declaring ABI v0–v3. The loader accepts notes 0–4 and rejects 5.

Arguments use rdi, rsi, rdx, r10, r8, r9; the full signed 64-bit result is in rax.
The private verification syscall 0x7e64 exists only in VOS_EXEC_VERIFY builds
and authenticates the trusted Rust probe's process/thread identity.

## Catalog enumeration: 14

app_next(cookie, output, capacity) returns 48 for one visible record, 0 at
end, or a negative VOS error. Capacity must equal 48. Destination validation
covers all 48 writable user bytes, including end-of-enumeration calls; errors
and end do not alter output. Kernel-owned staging precedes copy_to_user.

| Field | Offset | Type |
|---|---:|---|
| version | 0 | u32, 1 |
| size | 4 | u32, 48 |
| app_id | 8 | u64 logical identity |
| next_cookie | 16 | u64 opaque cursor |
| name_length | 24 | u32, 1–15 |
| flags | 28 | u32, zero |
| name | 32 | u8[16], ASCII and zero-filled suffix |

C and repr(C) Rust assertions enforce 48 bytes and every offset. The owner's
layout decision omitted the extra reserved field in the original proposal,
which would have made the naturally aligned structure 56 bytes. There is no
padding or uninitialized public storage in the approved layout.

Cookie 0 starts enumeration. Nonzero cursors combine an authority generation
and bounded catalog position. Regrant invalidates old cookies (ESTALE);
invalid positions return EINVAL. Revoked or absent authority returns EACCES.
Generation exhaustion fails closed. IDs identify logical applications, never
Multiboot module indices. Enumeration reveals only the explicit whitelist.

## Synchronous execution: 15

app_run_foreground(app_id, flags) accepts flags=0 only. Foreground means
run-and-join. Console transfer requires separately installed trusted
FOREGROUND authority; it is not granted by flags=0. See the Increment 3
addendum below. A negative return is a pre-start
syscall error. A nonnegative result is kind in bits 32–63 and status in bits
0–31:

| Kind | Value | Status interpretation |
|---|---:|---|
| Exited | 0 | signed i32 child exit status |
| Faulted | 1 | trap vector (zero for a kernel kill without a recorded trap) |
| Cancelled | 2 | cancellation reason; 1 means kernel cancellation |

Thus exit -7 is packed as 0x00000000fffffff9, distinct from syscall error -7.
The optional vos_apps Rust crate provides checked next, decode and run APIs;
the existing vos crate and ABI v3 application linkage remain unchanged.

EINVAL covers zero IDs and nonzero flags; EACCES covers missing rights or
whitelist membership; EAGAIN covers the occupied slot/process-table exhaustion;
ENOMEM covers loader/thread allocation; EUNSUPP covers rejected ELF/policy.
Queued or loaded cancellation before start returns EINTR after rollback.
Once started, cancellation returns Cancelled only after join and destruction.
Revocation after DONE prevents later calls but does not change that completion.

## Authority and lifecycle

Trusted bootstrap installs separate ENUM and RUN rights, a whitelist and a
generation on a registered READY ABI v4 process before thread creation.
RUN alone permits diagnostic catalog policy only. Increment 3 adds an
explicit trusted FOREGROUND right for console-policy children. Authority binds a live
process pointer plus monotonic PID; it is neither transferable nor inherited.
A private verification refresh hook is absent from normal builds.

One fixed request slot stores scalar parent PID/TID/generation/app ID. The
parent blocks on completion without retaining a user pointer or manually
switching CR3. A kernel thread revalidates authority and ELF, loads an isolated
child with the existing loader, starts it, waits for its thread to be reaped,
and destroys its address space under kernel CR3. Completion becomes immutable
at DONE and the parent releases the slot after consuming it.

Cancellation is sticky until DONE. Revocation and proc_kill cancel an active
request and interrupt an uncooperative child. Parent termination cannot reclaim
parent storage while its request remains active. Failed load/start paths
reclaim once, and service shutdown joins its kernel thread. No production
execution deadline was added.

The existing limits remain eight processes, sixteen threads, one request and
one child. Children inherit no file, channel, execution or networking grants. The
Increment 2 baseline enabled a non-interactive diagnostic fixture. Increment 3
adds a separately installed exclusive console lease for an authorized child;
filesystem executables and background/recursive jobs remain unsupported.

See the [verification report](../results/app-exec.md).

## Increment 3: explicit foreground console lease

The public ABI remains v4: calls 14/15, the 48-byte enumeration record, flags=0
and completion encoding are unchanged. ABI v0-v3 dispatch remains unchanged.
Trusted bootstrap may install ENUM=1, RUN=2 and FOREGROUND=4 independently.
FOREGROUND requires RUN. Existing ENUM|RUN=3 grants keep diagnostic-only
behavior; they do not gain access to console-policy applications.

The common-image vsh declares ABI v4 and receives an explicit whitelist of
hello (logical ID 0x1002) and rixie (0x1003). The shell itself remains hidden
from that whitelist. Catalog names resolve to stable logical IDs, not module
indices or paths. Rixie remains ABI v3; hello remains ABI v0. Their accepted
ELF identities are unchanged.

Only a live current console owner with RUN|FOREGROUND may start a console
application. The broker suspends keyboard delivery, drains bounded pending
input, clears the completed-line/editor state and keyboard modifiers, then
installs a private child lease before starting the child. Input typed across
a handoff is intentionally discarded, not replayed to a different owner.
Untrusted non-owner debug output remains available in prefixed serial
diagnostics but cannot write the foreground display. Kernel diagnostics
remain available.

The parent blocks without reading the console. The child gets no inherited
file, channel, execution or network grants. Console access is explicitly
installed by the broker. Recursive execution remains denied. Legacy ABI v0
hello uses its existing debug-write path; its ABI gains no console syscalls.

After the child thread terminates and is reaped, the broker destroys its
address space under kernel CR3, then revalidates the parent PID/TID, liveness,
execution authority and console generation before restoring ownership.
Cancellation unwinds blocked console reads before reclamation. A revoked or
terminated parent is never restored; a surviving authorized parent can resume
after child exit, fault or cancellation. Failed load/start paths roll back.
Failure to restore safely fails closed. No production execution deadline is
introduced.

One request and one child remain bounded by the existing eight-process and
sixteen-thread limits. There is no terminal multiplexer, background job,
arbitrary executable path, application networking or filesystem ELF loader.

The private 0x7e65 handoff verification syscall exists only in instrumented
builds and authenticates the pinned probe process/thread. It is absent from
normal builds. See the [Increment 3 verification report](../results/app-handoff.md).

# VOS Native Application ABI v1 (x86_64)

| | |
|---|---|
| Status | **Experimental and unstable.** Implemented in N2.4 (2026-10-18). Any part may change; there is no compatibility promise. |
| Scope | What ABI v1 adds to ABI v0: four system calls (yield and channels), their error codes and the version negotiation. Everything not mentioned here is exactly ABI v0 (`docs/abi/syscall-v0.md`): entry state, register preservation, memory layout, executable format, faults. |
| Sources of truth | Kernel: `arch/x86_64/syscall.{h,c}`, `channel.{h,c}`, `usercopy.c`, `process.c`, `elf.c`. User: `user/include/vos/syscall.h`, `user/lib/syscall_v1.S`, `user/lib/abi_note_v1.S`. |

## 1. Versions

- An executable declares its ABI version in its VOS ABI note (`docs/abi/syscall-v0.md`,
  section 9): `0` or `1`. The loader accepts both and rejects any other value
  (`ELF_EABI`); the version is recorded in the process.
- An **ABI v0** executable keeps working unchanged (`hello.elf` is byte-for-byte the N1 file).
  For each ABI v1 system call (numbers 2–5) it gets `-VOS_ENOSYS`, exactly like an unknown
  number.
- An **ABI v1** executable links `abi_note_v1.o` and `syscall_v1.o` instead of `abi_note.o`
  (`user/user.mk`, programs `user/suite/ipc_*`). It may use every v0 and v1 call.

## 2. System calls

Entry, registers and results as in ABI v0: `int $0x80`, number in `rax`, arguments in `rdi`,
`rsi`, `rdx`; result in `rax` (`>= 0` success, `-VOS_E*` error); **every other register,
`rflags` and the user stack are preserved**, also when the call blocks or the process is
preempted meanwhile.

| No. | Name | Arguments | Result |
|---|---|---|---|
| 2 | `yield` | — | `0`: the thread gave up the CPU; it continues when its turn comes |
| 3 | `channel_send` | `rdi` handle, `rsi` buffer, `rdx` length (0–256) | `0`, or an error; never blocks |
| 4 | `channel_recv` | `rdi` handle, `rsi` buffer, `rdx` capacity | the message length (0–256), or an error; blocks while the queue is empty and the peer is open |
| 5 | `handle_close` | `rdi` handle | `0` or `-VOS_EBADHANDLE` |

User wrappers: `vos_yield`, `vos_channel_send`, `vos_channel_recv`, `vos_handle_close`
(`<vos/syscall.h>`).

## 3. Error codes

| Name | Value | Meaning |
|---|---|---|
| `VOS_ENOSYS` | 1 | unknown call, or a v1 call from an ABI v0 executable |
| `VOS_EINVAL` | 2 | `channel_send` length above 256 |
| `VOS_EFAULT` | 3 | the buffer is not readable (send) or writable (receive) user memory |
| `VOS_EAGAIN` | 4 | `channel_send`: the peer's queue is full (16 messages) |
| `VOS_EPIPE` | 5 | the peer endpoint is closed (send; receive once the queue is empty) |
| `VOS_EBADHANDLE` | 6 | not a valid handle of this process |
| `VOS_ENOMEM` | 7 | a kernel limit was reached (kernel-side channel creation only, so far) |
| `VOS_EMSGSIZE` | 8 | `channel_recv`: the capacity is smaller than the oldest message |
| `VOS_EACCES` | 9 | the handle lacks the right for this operation |

## 4. Channels and handles

- A **channel** has two endpoints; each endpoint belongs to one process through one handle.
  Each endpoint has a receive queue for the messages its peer sent: at most **16** messages
  of at most **256** bytes, kept in kernel memory (copied in on send, copied out on receive;
  no memory is shared between processes and no kernel address is ever exposed).
- In ABI v1 programs **do not create channels**. The kernel creates them from trusted
  configuration (the GRUB test configuration in N2.4) and installs one handle per end. A
  process's handles are numbered in the order they were installed: the first is `1`.
- A **handle** is a small positive number that is only meaningful in the process that holds
  it (another process's equal number names nothing, or its own, different endpoint). After
  `handle_close` the number is dead: a later handle in the same slot gets a different number
  (a 16-bit generation is part of it), so a stale number cannot reach a new channel.
- **Rights** are fixed at creation: send and/or receive. An operation without the right
  returns `-VOS_EACCES`.

### Exact semantics

Checks happen in this order; the first that fails decides the result.

`channel_send(h, buf, len)`: invalid `h` → `-VOS_EBADHANDLE`; no send right → `-VOS_EACCES`;
`len > 256` → `-VOS_EINVAL`; peer closed → `-VOS_EPIPE`; peer queue full → `-VOS_EAGAIN`;
`buf` not readable → `-VOS_EFAULT` (nothing queued); otherwise the message (also of length 0)
is queued and a receiver blocked on the peer is woken; `0`.

`channel_recv(h, buf, cap)`: invalid `h` → `-VOS_EBADHANDLE`; no receive right →
`-VOS_EACCES`; if a message is queued: `cap` smaller than its length → `-VOS_EMSGSIZE`, `buf`
not writable for that length → `-VOS_EFAULT` (in both cases the message **stays queued**,
nothing is truncated or lost), else it is copied out and its length returned. If the queue is
empty: peer closed → `-VOS_EPIPE`; else the thread **blocks** until a message arrives or the
peer closes, then re-checks. Messages queued before the peer closed are still delivered first.

`handle_close(h)`: invalid `h` (including a second close) → `-VOS_EBADHANDLE`; otherwise the
endpoint closes: messages still queued to it are discarded, a receiver blocked on the peer is
woken (it drains, then gets `-VOS_EPIPE`), and when both ends are closed the channel is freed.

**Process end.** When a process exits, faults or is killed, all its handles are closed first,
exactly as by `handle_close`. A process blocked in `channel_recv` can be killed by the kernel:
it is taken off the wait queue and ends without returning to user mode.

## 5. Memory validation

`channel_send` reads the buffer with `copy_from_user` and `channel_recv` writes it with
`copy_to_user`: the whole range must lie in the user half without overflow, every page must
be a present 4 KiB page that is user-accessible at every level (and, for receive, writable
at every level); all pages are checked before the first byte is copied, and the bytes go
through the kernel's alias of the validated frames, never through the user address. A
failed check changes nothing. Length 0 never examines the pointer.

## 6. Scheduling

`yield` and a blocking `channel_recv` give the CPU to other threads; the timer may also
preempt the program at any instruction (N2.3). All of this is invisible to the program
except as time passing. The kernel itself is not preemptible and runs every system call with
interrupts disabled, so a check like "the queue is empty, block" cannot lose a wakeup.

## 7. Not in ABI v1 (yet)

Channel creation by programs, handle transfer between processes, waiting on several handles,
timeouts, non-blocking receive, `spawn`/`wait`, shared memory, `SYSCALL`/`SYSRET`.

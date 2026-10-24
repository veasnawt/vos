# ABI v3: explicitly granted text console

ABI v3 adds console calls to ABI v2. Executables carry VOS note version 3 and
use the same static ELF64 profile, `int 0x80` register convention, user entry
state, protected memory and C calling convention. Calls 0–10 and all existing
error numbers retain their meanings. ABI v0/v1/v2 applications still receive
`-VOS_ENOSYS` for calls 11–13. Version 4 is currently unsupported.

| Number | Call | Arguments | Success |
| --- | --- | --- | --- |
| 11 | `console_read(destination, capacity)` | rdi: writable user buffer; rsi: capacity 1–129 including NUL | completed line length 0–128, with a NUL terminator |
| 12 | `console_write(source, length)` | rdi: readable user buffer; rsi: length 0–256 | number of bytes consumed |
| 13 | `console_clear()` | unused registers ignored | 0 |

The entry number is in rax and the result replaces rax. Other registers and
RFLAGS retain the existing syscall contract. Negative results are VOS errors,
not Linux errno values. Public C declarations/stubs are in
`user/include/vos/console.h` and `user/lib/syscall_v3.S`; link the v3 note once.
The same stubs are callable from Rust through `extern "C"`.
Shared error constants are available in `vos/syscall.h` and `vos/file.h`.

## Authority and lifetime

Only one explicitly granted, live process may use the console. Trusted setup
grants it before process start and binds the session to both its process record
and never-reused PID. Merely declaring ABI v3 grants no authority. Missing,
foreign or revoked authority returns `-VOS_EACCES` before pointer handling.
Exit, fault and destruction revoke the session; kill while waiting returns
through the existing process cancellation/termination path. No grant is
inherited or available through a user grant syscall.

Filesystem authority remains separate. A console grant does not grant files.
The opt-in shell launcher may explicitly provision index 0 at the non-root
`shell` directory of an attached trusted read-only volume. Normal hello receives
neither console authority nor filesystem grants.

## Read contract

The caller owns a separate destination buffer. A read waits for Enter using the
existing completed-line reader, PS/2 driver and scheduler/PIT waits. It does not
enable interrupts in ordinary kernel code. The shell writes its own prompt;
the kernel echoes accepted keys and performs bounded Backspace editing.

Capacity 0 or greater than 129 returns `-VOS_EINVAL`. The entire requested
destination is validated as user-writable before waiting and again after waiting,
using the live trap-frame arguments. Invalid/noncanonical, unmapped, kernel and
read-only pointers return `-VOS_EFAULT` with no destination bytes changed.

Only a complete valid line can be copied. Empty input returns 0 and writes one
NUL byte. Success writes exactly length+1 bytes, leaving the remainder untouched.
If capacity cannot hold the completed line plus NUL, `-VOS_EMSGSIZE` leaves the
destination unchanged and retains the line for retry. Post-wait pointer errors
also retain it. Cancellation returns `-VOS_EINTR`; unavailable input returns
`-VOS_EUNAVAIL`. Overlong keyboard submissions are discarded in full and reported
by the existing reader before it starts a fresh line. No truncated prefix reaches
the application.

There is one reader/consumer per granted process. This is a synchronous line
interface, not a raw-key event stream, PTY, ANSI terminal or input multiplexor.

## Write and clear contract

Length greater than 256 returns `-VOS_EINVAL`. The kernel copies all source bytes
privately before emitting anything; an invalid range returns `-VOS_EFAULT` with
no display output. Zero length ignores the source pointer and returns 0.

Printable ASCII and newline are displayed; other bytes become `?`. There is no
escape-sequence interpreter. Display output is unprefixed for the granted app,
while the serial diagnostic stream retains the existing `user[name]:` framing.
An application therefore cannot use console_write to emit unprefixed trusted
kernel verdicts into that diagnostic stream. Keyboard echo remains input text.

Clear resets the opt-in text area and its character shadow. It does not erase
serial diagnostics or change the status/panic protections. Without a usable
display it is a successful no-op.

## Native Rust foundation

`user/vsh/main.rs` is `no_std`/`no_main`, uses no allocator and exports a C-ABI
`main`. The existing assembly entry/exit runtime, syscall stubs and ELF linker
script are reused. Rust builds a freestanding static library so required core
routines resolve; unused sections are removed when linking the final VOS ELF.
No host libc, Linux runtime, TLS, dynamic loader or second keyboard system is
linked. Small in-tree C memory routines supply the compiler's memory operations.

The installed `x86_64-unknown-none` target uses the System V C convention without
a red zone and avoids vector/floating-point registers by default, as documented
in the [Rust target reference](https://doc.rust-lang.org/rustc/platform-support/x86_64-unknown-none.html).
The VOS build explicitly selects the small code model, static relocation and
abort-on-panic. The existing ELF validator still enforces W^X, address/size bounds,
one ABI note, no relocations/TLS and no floating-point/vector instructions.

See [native vsh verification](../results/n4-native-vsh.md) for host checks, actual
ring-3 C/Rust programs, firmware results and screenshots. These records establish
the first bounded native shell, not physical-hardware or general POSIX support.

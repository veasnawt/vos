# VOS Native Application ABI v0 (x86_64)

| | |
|---|---|
| Status | **Experimental and unstable.** Implemented in N1 (commits up to `74501a7`). Any part may change without notice; there is no compatibility promise of any kind. |
| Scope | The contract between a VOS user program and the kernel on `ARCH=x86_64`: entry state, system calls, memory layout, executable format. |
| Version marker | The VOS ABI note in every executable carries ABI version `0`. A later, incompatible ABI will use a new version number, and the loader will reject versions it does not implement. |
| Sources of truth | `arch/x86_64/syscall.{h,c}`, `usercopy.c`, `usermode.S`, `process.c`, `elf.c`, `loader.c`; `user/include/vos/syscall.h`, `user/lib/{crt0.S,syscall.S,abi_note.S,user.ld}` |

Applications should call the runtime functions `vos_exit` and `vos_debug_write`
(`user/include/vos/syscall.h`) instead of issuing system calls themselves. The raw interface
below is documented so it can be reviewed and tested; it is not a stable public API.

---

## 1. Calling convention

- User code follows the System V AMD64 psABI for its own function calls (`rdi, rsi, rdx, rcx,
  r8, r9` arguments; `rax` result; `rbx, rbp, r12–r15` callee-saved; 16-byte stack alignment
  before every `call`; a 128-byte red zone below `rsp`; DF clear at calls and returns).
- The kernel never writes to the user stack (an interrupt from ring 3 switches to the kernel
  stack in TSS.RSP0), so the red zone is safe across system calls and faults.

## 2. Process entry state

The kernel enters a new process exactly once, with `iretq`:

| Item | Value |
|---|---|
| `rip` | `e_entry` of the executable |
| `rsp` | `0x00007FFFFFFFEFF0` — 16-byte aligned; the 16 bytes at `rsp` are zero |
| General-purpose registers | `rax, rbx, rcx, rdx, rsi, rdi, rbp, r8–r15` all zero |
| `rflags` | `0x2`: IF = 0, IOPL = 0, DF = 0, AC = 0, TF = 0 |
| `cs` / `ss` | `0x23` / `0x1B` (ring 3, 64-bit code; user data) |
| `ds, es, fs, gs` | null selectors (`0`); FS and GS bases are 0 |
| Floating point | not available (section 7) |
| Arguments | none: no `argc`, `argv`, environment or auxiliary vector |
| Interrupts | disabled for the whole life of the process (there is no timer and no preemption) |

The runtime's `crt0.S` records the entry `rsp` in `__vos_entry_rsp`, clears `rbp`, keeps `rsp`
16-byte aligned, calls `int main(void)` and passes its return value to `vos_exit`. A test
program checks this state directly with its own `_start` (`user/suite/regs.S`).

## 3. System-call entry: `int $0x80`

- The system-call number goes in `rax`; arguments go in `rdi, rsi, rdx, r10, r8, r9` (the 4th
  argument is `r10`, not `rcx`); the result comes back in `rax`.
- Vector `0x80` is the only IDT gate with DPL 3. `int n` for any other vector from ring 3 raises
  `#GP(n * 8 + 2)`, which ends the process.
- The CPU switches to the kernel stack in TSS.RSP0 and pushes `ss, rsp, rflags, cs, rip`. The
  kernel saves every register, clears DF, and clears AC when SMAP is enabled.
- **Preserved:** every general-purpose register except `rax`, `rflags` (including CF, DF and AC
  as the program left them) and the whole user stack. A non-terminating call resumes at the
  instruction after `int $0x80` in the same process, with the CPU-pushed frame restored by
  `iretq`; only `rax` differs. This is verified by tests that load every register, set CF and DF
  and place red-zone canaries before calling.
- **Validation before returning:** CS `0x23`, SS `0x1B`, IF = 0, IOPL = 0, canonical lower-half
  `rip`/`rsp`, the process's page tables still active. A violation is a kernel error (panic).
- `int $0x80` from ring 0 is a kernel error (panic).

## 4. Results and errors

A result is a signed 64-bit value in `rax`: `>= 0` is success; a negative value is
`-VOS_E<name>`. These numbers are VOS's own, **not Linux errno values**, and there is no `errno`.

| Name | Value | Meaning |
|---|---|---|
| `VOS_ENOSYS` | 1 | unknown system-call number |
| `VOS_EINVAL` | 2 | invalid argument (e.g. length above the limit) |
| `VOS_EFAULT` | 3 | a buffer is not readable user memory |

## 5. System calls

| `rax` | Name | Arguments | Result |
|---|---|---|---|
| 0 | `exit` | `rdi`: status; the low 32 bits, signed | does not return |
| 1 | `debug_write` | `rdi`: buffer, `rsi`: length | `length`, `-VOS_EINVAL` or `-VOS_EFAULT` |
| any other | — | ignored | `-VOS_ENOSYS`; the process continues |

### `exit`
Ends the process. The kernel records the status, returns to its own code on its own page tables
and releases every frame and page table of the process. The process is never resumed.

### `debug_write`
1. `length > 256` → `-VOS_EINVAL` (checked first, before the pointer).
2. `length == 0` → `0`; the pointer is not examined.
3. The bytes are copied into a kernel buffer with `copy_from_user` (section 6); any invalid
   byte → `-VOS_EFAULT` and nothing is printed.
4. Only the kernel copy is printed, on the serial console, as one or more lines
   `user[<name>]: <text>`. `<name>` is chosen by the kernel (for the boot application it is the
   GRUB module's command line, `hello`). A newline ends a line, and every following line gets the
   prefix again; every byte outside printable ASCII (`0x20–0x7E`), including carriage return and
   escape sequences, is printed as `?`; the output always ends at a line end.
5. The result is `length`.

User output can never produce a line that does not start with `user[`. Trusted kernel reports
use other prefixes (e.g. `proc[hello]: exited status 0, all frames reclaimed`), and the test
harnesses match kernel markers only in lines that do not start with `user[`.

## 6. User-pointer validation (`copy_from_user`)

For a user range `[ptr, ptr + len)`:

- `len == 0` succeeds without examining `ptr`.
- The range must not overflow and must lie in `[0x400000, 0x0000800000000000)`: null, the null
  guard, non-canonical addresses and the whole kernel half are rejected.
- Every 4 KiB page the range touches must be present at all four page-table levels, carry the
  user bit at all four levels (the effective permission), be a 4 KiB page and be backed by a
  frame below 1 GiB. All pages are checked before the first byte is copied.
- The kernel then reads the bytes through its own supervisor-only alias of the validated
  frames. It never dereferences the user address, so the copy cannot fault and does not depend
  on SMAP.
- This is race-free only because N1 runs one process at a time on one CPU with interrupts
  disabled and has no demand paging or shared memory.

There is no `copy_to_user` yet; no system call writes to user memory.

## 7. Memory and CPU protection

### User address space

| Range | Contents |
|---|---|
| `0x0` – `0x3FFFFF` | never mapped (null guard) |
| `0x400000` – `0x3FFFFFFF` | the executable's `PT_LOAD` segments (at most 1 MiB in total) |
| `0x7FFFFFFFA000` – `0x7FFFFFFFAFFF` | never mapped (stack guard page) |
| `0x7FFFFFFFB000` – `0x7FFFFFFFEFFF` | stack, 16 KiB, read/write, no-execute |
| `0x7FFFFFFFF000` – `0x7FFFFFFFFFFF` | never mapped |
| upper half | the kernel: mapped, but supervisor-only at every level |

- Every page is a fresh, zeroed frame. Code segments are read + execute and never writable;
  read-only data is read-only and no-execute; writable data and the stack are no-execute. No
  page is ever writable and executable.
- Ring 3 runs with IOPL 0 and there is no I/O permission bitmap: every port access, every
  privileged instruction (`cli`, `hlt`, `mov %cr3`, `wrmsr`, ...) raises `#GP`.
- On CPUs that have them, SMEP, SMAP and UMIP are enabled. With UMIP, the descriptor-table and
  machine-status instructions raise `#GP(0)` in ring 3 (tested: `sgdt`, `sidt`, `smsw`, `str`;
  `sldt` is covered by the architecture but not tested). On the default QEMU `qemu64` model none
  of the three exists.

### Faults
Any exception raised in ring 3 ends the process; the kernel records the vector, error code,
`rip`, `cs`, `rflags`, `ss:rsp` and, for `#PF`, `cr2`. There are no signals and no user fault
handlers. A fault in kernel mode is a kernel panic.

### Floating point and SIMD
Not available. `CR0.EM = 1`: x87 instructions raise `#NM`, MMX and SSE instructions raise `#UD`;
either ends the process. There is no FPU state to save or restore. (Under QEMU TCG, `emms` is not
trapped, an emulator deviation; it only resets the x87 tag word.)

## 8. Executable profile (ELF64)

The kernel loader (`arch/x86_64/elf.c`) enforces the following before allocating any memory;
`scripts/check-user-elf.sh` enforces the same rules at build time. Every 64-bit field read must
be below 2^40.

| Item | Requirement |
|---|---|
| File | 64 bytes to 1 MiB |
| `e_ident` | `\x7fELF`, ELFCLASS64, ELFDATA2LSB, EV_CURRENT, `EI_OSABI` 0, `EI_ABIVERSION` 0, padding zero |
| Header | `ET_EXEC` (ET_DYN/PIE rejected), `EM_X86_64`, `e_version` 1, `e_flags` 0, `e_ehsize` 64, `e_phentsize` 56, 1–8 program headers at an offset >= 64, 8-byte aligned, inside the file |
| `PT_LOAD` | 1–4; `p_align` 0x1000; `p_vaddr` and `p_offset` page-aligned; `0 < p_memsz`; `p_filesz <= p_memsz`; file range inside the file; inside `[0x400000, 0x40000000)`; sorted, page ranges disjoint; flags only R/W/X with R required; never W and X together; at least one executable segment; at most 1 MiB in total |
| Entry | inside the file-backed part of an executable `PT_LOAD` |
| `PT_NOTE` | `p_align` 4, inside the file, a well-formed sequence of notes, exactly one VOS ABI note |
| `PT_GNU_STACK` | allowed only without `PF_X` |
| Everything else | rejected: `PT_NULL`, `PT_INTERP`, `PT_DYNAMIC`, `PT_TLS`, `PT_PHDR`, `PT_SHLIB`, `PT_GNU_RELRO`, `PT_GNU_PROPERTY`, `PT_GNU_EH_FRAME`, unknown types |
| Section headers | ignored by the loader |

Loading: each `PT_LOAD` page is a fresh zeroed frame that receives exactly the segment's file
bytes; the rest stays zero. The module the file came from is never mapped into the process.

**Build-time-only rules** (they need section headers or disassembly, so the loader cannot check
them): no relocation, dynamic or TLS sections; no `%fs`/`%gs` operands; no x87/MMX/SSE
instructions. A file that evades them is still confined: FP/SIMD instructions trap, FS/GS bases
are 0, and unapplied relocations only break the program itself.

The reference build (`user/user.mk`) uses `-ffreestanding -nostdinc`, `-fno-pic -fno-pie`,
`-mgeneral-regs-only`, `-fno-stack-protector`, `-fcf-protection=none`, no unwind tables, and
`ld -nostdlib -static -z noexecstack -z max-page-size=0x1000 -z norelro --build-id=none` with
`user/lib/user.ld` (three page-aligned `PT_LOAD`s at `0x400000`: R-X, R, RW).

### VOS ABI note

In the GNU toolchain's form (three 32-bit words, then name and descriptor padded to 4 bytes):

| Field | Value |
|---|---|
| `n_namesz` (u32) | 4 |
| `n_descsz` (u32) | 4 |
| `n_type` (u32) | 1 (`VOS_NOTE_ABI`; types are scoped by the owner name) |
| name | `"VOS\0"` |
| descriptor | u32 ABI version: `0` |

The loader finds it by walking `PT_NOTE` segments, never through section headers. A file
without exactly one valid VOS ABI note is rejected. The runtime provides it (`abi_note.S`), and
`user.ld` refuses to link a program without exactly one.

## 9. How an application reaches the kernel in N1

GRUB delivers the executable as a Multiboot2 module (`module2 /boot/hello.elf hello`). At boot
the kernel runs the one module whose command line is exactly `hello`: none means nothing runs;
more than one means none runs. This selection rule is an N1 loader policy, not part of the ABI.

## 10. Not provided by ABI v0

No memory allocation, files, time, threads, signals, processes, IPC, environment, TLS, dynamic
linking, PIE/ASLR, FPU/SIMD, `copy_to_user`, shared libraries or C library. No interrupts reach a
user program, and nothing preempts it: a program that never calls `exit` keeps the CPU forever.

## 11. `int $0x80` today, `SYSCALL` later

ABI v0 uses `int $0x80` because it reuses the IDT, the TSS stack switch and the exception path
that already exist, and it saves and restores the full frame. A future `SYSCALL`/`SYSRET` entry
would differ in ways that are visible to programs and to the kernel:

- `SYSCALL` overwrites `rcx` (return `rip`) and `r11` (`rflags`), so those could no longer be
  preserved — ABI v0's "everything but `rax`" promise would change.
- `SYSCALL` does not switch stacks; the kernel would need `swapgs` and a per-CPU stack pointer,
  and `SYSRET` needs a canonical-address check of the return `rip` before use.
- The selector layout (kernel 0x08/0x10, user data 0x18, user code 0x20) was chosen to fit
  `STAR`, but this has not been tested.

Such a change would be a new ABI version in the VOS note, not a silent change to v0.

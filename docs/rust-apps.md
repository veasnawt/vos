# Writing a native Rust application

VOS applications use the existing static x86-64 ELF profile and system-call ABI.
The small `vos` source SDK supplies shared bindings, an entry macro and the panic
convention. There is no allocator, Cargo dependency tree, hosted libc or separate
Rust application architecture. `vsh` and the independent `rust-demo` use this same
SDK, C startup, syscall stubs and linker script. The independently launched
[Rixie Core v0](rixie-core.md) also uses this SDK with an explicit console-only
grant; it does not import shell internals or add an application framework.

## Smallest program

Create `user/my-app/main.rs`:

```rust
#![no_std]
#![no_main]

vos::entry!(run);

fn run() -> i32 {
    if vos::debug_write(b"Hello from my Rust application!\n").is_err() {
        return 1;
    }
    0
}
```

Build with a Rust compiler that has `x86_64-unknown-none` installed:

```sh
make rust-app RUST_APP=my-app RUSTC=/path/to/rustc
scripts/check-user-elf.sh build/n4-rust/user/my-app.elf
```

The output is `build/n4-rust/user/my-app.elf`. Change `RUST_BUILD` to choose a
separate build tree. `RUST_BUILD` must be nonempty. Application names identify
directories under `user/`; use simple names such as `my-app`. No manifest,
application-specific runtime, kernel include or custom linker script is needed.
Rust dependency files track module/source changes for incremental builds.

The build creates a shared `rust/libvos.rlib`, compiles the application as a
freestanding static library and links it with existing ABI v3 C/assembly stubs.
The target avoids floating-point/vector registers and a red zone; the build
selects the small code model, static relocation, panic abort and unused-section
removal. The [native ABI specification](abi/syscall-v3.md) and existing ELF
validator remain authoritative.

## Runtime conventions

- `vos::entry!(run)` exports the C-ABI `main` called by the existing assembly
  `_start`. `run` takes no arguments and returns an `i32` process exit status.
- The SDK supplies one panic handler, which exits with status 99. Do not define
  a second handler. `vos::exit(status)` also provides explicit termination.
- There are no argc/argv/environment values, TLS, heap, threads or automatic
  constructors. Use fixed stack/static storage and the existing ABI contracts.
- The executable carries VOS note 3. This provides the ABI v0–v3 call set, not
  authority. The kernel still decides grants and validates user memory.
- The shared C memory routines live in `user/rust/runtime.c`. The former vsh
  source path forwards to it for compatibility. The kernel is not linked into
  applications, and applications are not linked into the kernel.

## Shared bindings

| SDK surface | Existing ABI calls |
| --- | --- |
| `exit`, `debug_write` | ABI v0 exit/debug diagnostics |
| `yield_now`, `close`, `channel::{send, receive}` | ABI v1 scheduling and owned channel handles |
| `file::{initial, open_at, read_at, stat, next}` | ABI v2 read-only filesystem handles |
| `console::{read_line, write, write_all, clear}` | ABI v3 explicitly granted console |
| `raw` | Unsafe C declarations for the same assembly stubs, without duplicated syscall numbers |

The slice-based wrappers return `vos::Result<T>`. `vos::Error` stores a positive
VOS error number; the raw C ABI returns its negative value. Error values 1–20
match existing VOS definitions and are not Linux errno values. Receive/read
wrappers check returned lengths before exposing them. Console reads also check
the NUL terminator, and `write_all` uses bounded 256-byte chunks.

`file::Stat` and `file::Entry` are `repr(C)` wire records with compile-time size
and offset checks: 64 and 272 bytes respectively. `file::next` returns `None` at
EOF. Handles remain process-owned and must be closed explicitly; paths and
rights are passed to the existing kernel validators without narrowing rights.

Use `raw` only when the caller can uphold pointer validity, object lifetime and
size contracts. Rust does not replace kernel checks or grant capabilities.
Existing C applications continue to use their public headers and identical
assembly stubs. The kernel/user boundary is the C/wire ABI, not Rust's internal ABI.

## Run the independent example

```sh
make rust-demo-run RUSTC=/path/to/rustc
# UEFI image, using the existing EFI GRUB/mtools prerequisites:
make rust-demo-uefi-iso RUSTC=/path/to/rustc
```

`user/rust-demo/main.rs` uses public SDK APIs only. It prints diagnostics, yields,
then checks denial of console, filesystem and channel operations without grants.
It returns status 0 and is reclaimed. Its standalone ISO loads no `vsh` module.
The example uses the existing trusted module loader; no new application loader
or syscall family was added.

Another unprivileged Rust program can also use the original boot application
slot, without editing the kernel. After the `my-app` build above:

```sh
make ARCH=x86_64 BUILD=build/n4-rust/custom-kernel \
  USER_BUILD=build/n4-rust/user \
  ARCH_ISO_FILES=build/n4-rust/user/my-app.elf:boot/hello.elf iso
```

The existing GRUB configuration calls that launch slot `hello`; this is a trusted
module label, not a dependency on hello's implementation. It grants no console,
filesystem or channels. Programs requiring authority need explicit trusted
provisioning, as native `vsh` already demonstrates.

See the [foundation results](results/n4-rust-foundations.md) for host checks,
loader validation, BIOS/UEFI evidence, screenshots and N4 readiness.

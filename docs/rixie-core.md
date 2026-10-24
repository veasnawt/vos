# Rixie Core v0

Rixie has a small, real home inside VOS: an independent Rust ring-3 application
with a keyboard-driven text interface. **This is a deterministic offline
prototype, not a conversational AI model.** Every response comes from a small
table in `user/rixie/dialog.rs`. There is no network, cloud service, model,
learning, command execution or hidden host-side responder.

## Run

Use the same compiler and installed `x86_64-unknown-none` target as native `vsh`:

```sh
make rixie-run RUSTC=/path/to/rustc
```

This builds `build/n4-rixie/user/rixie.elf` and a dedicated BIOS ISO, then opens
QEMU. Type into its display window. `RIXIE_BUILD` selects a separate output tree
and must be nonempty. No data disk or network device is attached. To build the
UEFI ISO with the existing GRUB EFI/mtools prerequisites:

```sh
make rixie-uefi-iso RUSTC=/path/to/rustc
```

The image is `build/n4-rixie/kernel/vos-uefi.iso`. Boot it using OVMF and a fresh
copy of its variable store as documented for the other VOS UEFI configurations.
The focused [test driver](../scripts/test-rixie.py) records the exact commands
and automates both firmware interactions.

To build just the application through the reusable SDK workflow:

```sh
make rust-app RUST_APP=rixie RUSTC=/path/to/rustc
```

That command places the ELF in `build/n4-rust/user/rixie.elf`; it does not grant
console access or launch the program. The dedicated Rixie boot configuration
provides the explicit trusted console grant.

## Interface

| Input | Behavior |
| --- | --- |
| `hello`, `hi` | Greeting |
| `help` | Supported topics and command explanations |
| `about` | Identity, native Rust implementation and deterministic limits |
| `questions` | Five predefined questions to try |
| `explain help`, `clear`, `echo`, `pwd`, `ls`, `cd`, `cat`, `exit` | Explain the named vsh command; prefix each name with `explain` |
| `what is VOS?` | Brief project description |
| `are you AI?` | Explicitly identifies the fixed-response prototype |
| `are you online?` | Explains offline operation |
| `what can you do?` | Lists the prototype's small scope |
| `who made you?` | Describes the project's human direction and coding assistance |
| `exit` | Goodbye, status-0 process teardown and the normal kernel halt |

ASCII letter case and surrounding whitespace are ignored. The five predefined
questions also accept omission of the final question mark. Empty input simply
shows another prompt. Other input receives a clear fallback; it is not interpreted
as a shell command. `exit now` does not exit. The existing console supplies
Backspace, Enter and the 128-byte line bound; Rixie adds no input driver.

For example, an actual session looks like:

```text
Rixie> hello
Hello! I'm Rixie, at home in VOS.
Rixie> are you AI?
No. Rixie Core v0 selects fixed replies; I am not a conversational AI model.
Rixie>
```

## Launch and authority

The existing trusted boot-module loader loads `rixie.elf` when the opt-in GRUB
configuration supplies `vos.rixie=1`. A small selector invokes the existing
`run_module` path with console access and no filesystem grant. No channels are
provisioned. Existing ELF checks, address-space isolation, syscall validation,
PID-bound console ownership, teardown and capability restrictions apply unchanged.

Rixie uses the shared `vos` SDK entry macro, bounded console slice wrappers,
runtime, syscall stubs and ABI v3 profile. There is no new ABI or kernel subsystem.
The kernel does not contain the reply table or parse Rixie's commands.

Rixie and `vsh` are separate opt-in boot configurations. `vsh` has no arbitrary
external-program launch command: typing `rixie` there does not launch this app.
Exit does not return to an unimplemented shell launcher; it ends the standalone
session. The existing shell source, command behavior and dynamic prompt are
unchanged. Use `make vsh-run` to boot the shell independently.

See [focused results and authentic BIOS/UEFI screenshots](results/n4-rixie-core.md).

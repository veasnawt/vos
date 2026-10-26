# N6.3 native mouse and pointer input

N6.3 passed focused hosted checks and actual QEMU BIOS/UEFI interaction on
`feat/n6-pointer-input`, based on `dfd292ca807370a0ceaf735d319fc1d15bd45370`.
This is a local review candidate. N6 is still in progress; no publication or
merge is performed by this increment.

## Implementation and authority

The existing single i8042 owner now demultiplexes keyboard and PS/2 auxiliary
bytes. The bounded three-byte decoder rejects invalid headers, overflow and
expired partial packets. Initialization validates ACK/BAT/standard mouse ID,
uses 1:1 scaling, and rolls back on failure. The existing sleeping GUI worker
polls both ports; no duplicate keyboard instance, IRQ worker or driver framework
is added.

The supervisor-only framebuffer sink draws a temporary geometric 8x12 cursor
using a fixed 384-byte underlay, signed-wide coordinate arithmetic, clipping and
pitch-aware reads/writes. Rust application pixels remain in an independent
ring-3 ELF. A process-owned pointer grant is opt-in; existing GUI grant 15 and
ABI v0-v5 layouts retain their behavior. The extension adds grant bit 16,
motion/button event types and syscall 21 for an overflow-recovery snapshot.
See [the pointer contract](../abi/pointer-v1.md).

One foreground owner, one <=256x160 surface, 40 backing frames, 4 KiB uploads
and a 16-event queue remain bounded. Hit testing, press capture, outside
release, revocation and loss recovery are explicit. No raw i8042, framebuffer,
DMA or network authority is exposed to applications. Cursor appearance is
independent of the event ABI and of any future icon library.

## Demonstrated failure and correction

The original top-right screenshot showed x=635, not the required x=1023.
A bounded diagnostic reused the exact validated BIOS image with QEMU packet
and controller-read traces. This was diagnosis of a known failure, not an
acceptance retry.

| Input | Packets generated and fully read | Remaining QEMU accumulator (mouse axes) |
|---|---|---|
| (-3000,-3000) | 5 packets, each (-127,+127) | (-2365,+2365) |
| (+3000,0) | 5 packets, each (+127,+127) | (0,+1730) |
| zero-relative synchronization | 5 packets, each (0,+127) | (0,+1095) |

All generated packet bytes matched the independently decoded guest reads.
The opposing input cancelled the un-emitted X remainder before packetization.
The resulting x=635 image is byte-identical to the previous failed screenshot.
Reading the generated queue alone did not request another synchronization.
This agrees with [QEMU 10.2.1's PS/2 implementation](https://github.com/qemu/qemu/blob/v10.2.1/hw/input/ps2.c):
a 16-byte queue holds five standard packets; each axis is capped at 127 per
packet, and remaining displacement is retained until another mouse sync.

Only the host injector was corrected. It waits for every generated packet's
three bytes to be read, issues zero-relative synchronization events as needed,
and requires exact emitted displacement equal to the complete requested vector
before sending the next vector. Each action retains a 12-second deadline and
a 32-sync bound. The fixed 0.3-second corner sleeps were removed; no deadline
was increased. The exact corner coordinates and pixel assertions are unchanged.
For a 3000-unit vector, the native trace records 24 packets and four zero-relative
syncs, rather than accepting the first five packets as completion.

The earlier coordinate logger correction assembles a complete record in a
checked 64-byte stack buffer and makes exactly one debug syscall. It preserves
the message format, event ordering and pointer behavior. Sixteen formatting
cases, including zero and maximum u32 coordinates, passed. The rebuilt native
ELF reproduced byte-identically and passed shell and kernel validators.
No kernel or application change was required for the motion recovery.

## Verification

| Check | Result and evidence |
|---|---|
| Hosted service/driver ASan/UBSan | 987,802 assertions PASS |
| Hosted framebuffer renderer ASan/UBSan | 2,156,957 assertions PASS |
| Console-handoff ASan/UBSan | 49,499 assertions PASS |
| Combined hosted total | 3,194,258; baseline lines are not counted twice |
| Rust GUI/pointer layouts | 3 tests PASS |
| Logger formatting | 16 cases PASS |
| Atomic request transport | 11 preserved checks PASS |
| Shared-input image adapter | 11 preserved checks PASS |
| Motion transport model | Six complete vectors, two buttons, corrupted-read rejection PASS |
| BIOS native prelude | Original 16 cases, 18 injected ELF failures, exhaustion rollback and checkerboard keyboard checks reused by exact source/artifact identity |
| BIOS normal media | Full movement, four exact corners, repeated clicks, a/b/q, held-button exit, cleanup and shell/hello/Rixie PASS |
| UEFI verification media | All 16 native GUI/pointer cases, 18 injected ELF failures, overflow, cancellation, full corners/clicks, cleanup and shell/hello/Rixie PASS |
| UEFI normal media | Ordinary pointer/click/keyboard use, held exit, cleanup and shell/hello/Rixie PASS |

The BIOS evidence is explicitly composite: its original prelude remains a
preserved partial run, while the corrected injector completed remaining checks
on normal media. Historical failed runs remain failures. Unchanged kernel,
probe, renderer, runtime and application hashes justify reuse; the three
sanitizer executables and Rust layout suite were not rebuilt unnecessarily.

The independent Windows-side audit verified all **281 manifest entries** across
six immutable output roots and all 49 tested source identities.
[The evidence index](n6-pointer/evidence.json) records manifests, image hashes,
source hashes, input events, trace identities and the exact reuse relationship.
[The runtime result](n6-pointer/result.json), [host summary](n6-pointer/host.txt),
[BIOS serial log](n6-pointer/bios.serial.txt), [UEFI serial log](n6-pointer/uefi.serial.txt)
and [37-file inventory](n6-pointer/changed-files.txt) accompany this report.

[BIOS screenshot](../images/n6/vos-pointer-bios.png) and
[UEFI screenshot](../images/n6/vos-pointer-uefi.png) are genuine QMP PNG captures
from the completed interactive sessions. Intermediate cursor, movement,
press/release and edge screenshots remain separate under
`build/n6-pointer-1/motion-continuation-1/{interactive-bios,uefi,interactive-uefi}/`.
Their paths, pixel oracles and hashes are in the evidence index. No mockups,
font replacements or branding assets were used.

## Storage, performance and preserved history

Limits remain 72 MiB cumulative execution, 96 MiB N6.3 and 3360 MiB aggregate.
Historical full reservations remain 880 MiB plus 96 MiB for N6.3; the remaining
margin after the original 1 MiB source forecast is **0.60546875 MiB**.
Accounting uses positive per-path allocated-byte high-water marks, positive
source growth and the existing 2 MiB Git provision, with no shrink credits.
The report/index retain the pre-finalization snapshot; final accounting is
recorded in `build/n6-pointer-1/final/motion-recovery-1/FINAL-RECEIPT.json`
and the completed guard records. No allocation was increased.

Compact Rust linking shares verified immutable target-runtime inputs; it does
not duplicate the runtime in two new staticlibs. Fresh debug-packed ELF copies
preserve the loader memory view and are deterministic; original ELFs and maps
remain unchanged. UEFI media used the stock GRUB producer with one immutable
private input tree and two fresh ISO assemblies through its supported xorriso
hook. Partition/boot/payload validators stayed strict. No GPT replay, manual
scratch reclamation or deletion credit was used.

Optimized hosted workload samples remain 64452516, 64523516, 64318416 ns.
They include assertions/startup and are separate from sanitizers; they are not
native input-latency or throughput measurements. Native end-to-end interaction
durations were 18.120 s (normal BIOS),
34.507 s (verification UEFI) and
15.616 s (normal UEFI), including screenshot
and host orchestration. No unsupported performance or physical-hardware claim
is made.

The incomplete atomic request, signedness stop, build-peak stops, initial logger
timeout, original x=635 clipping failure, isolation rejections and their
recovery receipts remain preserved. The accepted compact linking proof has
53 entries. The earlier N6.2 approximately 87-second unconfirmed monitoring
interval remains disclosed in its original evidence; uninterrupted isolation
is not claimed across that historical gap. Protected binaries/archives, all
13 worktrees, both stashes and historical records are checked again before
the local commit.

## Normal manual preview

Use the already-validated local images below. These are normal interactive
sessions with no verification deadline, no timed stress requirement and no
probe modules. Close one QEMU session before opening the other, and close the
preview terminal before asking the development controller to resume.

```bash
cd ~/src/myos
disk="$PWD/build/app-console-20261025T051241Z/attempt-1/disk.img"
bios="$PWD/build/n6-pointer-1/coordinate-log-continuation-1/normal-bios.iso"
uefi="$PWD/build/n6-pointer-1/motion-continuation-1/normal-uefi.iso"
common=(-machine q35 -accel tcg -m 128M -boot d -nic none
        -display gtk -vga none -device VGA,edid=on,xres=1024,yres=768
        -serial stdio -no-reboot -no-shutdown
        -drive "file=$disk,if=none,id=vs0,format=raw,readonly=on"
        -device virtio-blk-pci,drive=vs0,disable-legacy=on)

# BIOS
qemu-system-x86_64 "${common[@]}" -cdrom "$bios"
```

For UEFI, use the same variables/common arguments from above:

```bash
TMPDIR="$PWD/build/n6-pointer-1/tmp" \
qemu-system-x86_64 "${common[@]}" -cdrom "$uefi" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,snapshot=on,file=/usr/share/OVMF/OVMF_VARS_4M.fd
```

The firmware variable store uses an ephemeral QEMU snapshot, preserving the
installed OVMF file and all verified evidence stores. The manual command uses
a graphical window and serial stdout, and omits the recorded QMP/trace/log
destinations so no prior evidence is overwritten. These preview command
adaptations are distinct from the exact automated commands stored in the index.

The pointer demo starts on the native desktop. Move the mouse and use the
primary button: press turns its center amber and release toggles blue/green.
Use a/b to change the palette and q to release the GUI and return to vsh.
At the shell, try apps, hello and rixie; within Rixie use help/about and exit.
No timing-sensitive stress test is required. QEMU may grab the pointer;
Ctrl+Alt+G releases the grab with the GTK frontend.

For engineering reproduction, exact compiler/linker commands, pinned runtime
inputs, byte-identical build proof, image producer arguments and firmware
commands are retained in the indexed roots. The guarded test entry is a
checkpoint-bound continuation, not an unrestricted reusable build command:
do not rerun consumed attempt/controller identities or overwrite their outputs.
Fresh engineering execution requires the existing admission workflow and
cumulative accounting.

## Limits

QEMU verification does not establish physical x86_64 hardware compatibility.
Only standard i8042 PS/2 relative movement and the primary button are supported.
USB mice, wheels, extra buttons, hotplug, multiple surfaces, window management,
the final graphical Terminal, dock, VOS Display/VICons integration, application
networking and arbitrary GUI launching are not implemented. The existing
shell/launch authority and filesystem behavior remain intact.

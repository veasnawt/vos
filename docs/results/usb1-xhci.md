# USB.1: native xHCI discovery and input diagnostics

Local implementation, verified on QEMU 10.2.1 with SeaBIOS and OVMF. This is
controller discovery, **not USB keyboard or mouse support**. Physical computers
have not been tested. The baseline is
`cbe9f9202ca9a45aaafa57ec45fc01baeb347bc1`.

## Implementation and boundaries

`vos.usb_probe=1` enables a bounded PCI bus-0 scan for class/subclass/interface
`0c:03:30`. Without the exact option there is no USB scan or capability mapping.
At most four matching functions are inspected. A missing bus-0 device means
**not found on bus 0**, not absence of USB hardware elsewhere in the PCI topology.

The probe reads endpoint IDs, header type, command/status and BAR0/1. It accepts
assigned, page-aligned 32-bit or 64-bit memory BARs within the supported physical
address width, with firmware memory decoding already enabled. Existing
`devmap_check` and `devmap_map` supply supervisor-only, uncached device access;
RAM, reserved/firmware ranges, overlap and mapping failures retain their existing
rejections. Unsupported versus malformed inputs have separate outcomes.

Only seven capability DWORDs (28 bytes) are read. The snapshot validates
capability length, supported interface version (1.0, 1.1, 1.2), nonzero declared
slots/ports/interrupters, aligned doorbell/runtime offsets, nonoverlapping
register regions, bounded layout and address arithmetic. The declared layout
must fit a conservative 64 MiB bound. The probe never dereferences operational,
port, runtime, doorbell or extended-capability registers.

There is no destructive BAR sizing. Consequently the physical BAR aperture is
not established by the guest; layout arithmetic validation is not proof of that
aperture's size. QMP independently confirms a 16 KiB aperture for the tested
QEMU device. The fixed header mapping consumes one page per distinct controller
for the boot lifetime, at most four mappings in the existing eight-entry devmap
table. The mapping uses existing supervisor RW device pages; read-only describes
probe operations, not a new read-only page-table policy. No repeated probe API
is exposed to applications.

No controller reset, firmware ownership handoff, PCI configuration writes,
bus-master enablement, interrupt setup, rings, DMA setup, device enumeration or
USB transfer is performed. Firmware may already have enabled bus mastering;
the probe preserves it rather than claiming DMA is disabled. PCI command and
BAR values are compared before and after successful inspection. PCI mechanism-1
configuration-address selection still uses the existing I/O interface.

The implementation follows the capability-register model in Intel's
[xHCI specification](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf).
Unsupported versions, non-page-aligned BARs and controllers behind bridges need
future work. Even validated PCI metadata cannot guarantee that faulty physical
hardware will complete an MMIO read; USB.1 is opt-in and not hardware-certified.

## Input diagnosis

`vos.input_diag=1` independently enables serial stage/result messages for native
module lookup, ELF load, GUI worker startup, keyboard initialization and grants.
It is silent by default. Both existing text console grants and GUI console
grants depend on the same `keyboard_init` implementation. Keyboard I/O ordering,
ownership, cancellation, loader behavior and userspace ABIs are unchanged.

Disabling QEMU's i8042 while retaining xHCI and `usb-kbd` produces:

```text
input-diag: GUI input worker startup ok
input-diag: GUI ELF load ok
input-diag: GUI PS/2 keyboard initialization failed
input-diag: GUI authority grant failed
input-diag: native boot module present ok
input-diag: native ELF load ok
input-diag: text PS/2 keyboard initialization failed
input-diag: native console grant failed
native: vsh FAIL
```

These are expected negative-control outcomes on both firmware paths. They
demonstrate a PS/2 dependency in this configuration, not the cause of any
specific physical machine's startup failure. Merely attaching `usb-kbd` with
i8042 enabled does not prove USB input delivery.

## Verification

| Check | Result |
| --- | --- |
| xHCI host tests, ASan/UBSan | 45,713 assertions PASS |
| Existing keyboard/editor host tests, ASan/UBSan | PASS |
| Diagnostic default-silent/enable/disable test, ASan/UBSan | PASS |
| Existing console handoff host tests, ASan/UBSan | 49,499 assertions PASS |
| GUI bootstrap regression, ASan/UBSan | 523 assertions PASS; modeled service/scheduler boundaries |
| Kernel link/layout and Multiboot header | PASS |
| BIOS/UEFI ISO payload extraction | Five file hashes match build inputs in each of six images |
| Ordinary option-off GUI and shell interaction | BIOS and UEFI PASS |
| Probe, xHCI plus USB keyboard attached | BIOS and UEFI PASS; GUI startup, no USB input claim |
| Probe, no xHCI device | BIOS and UEFI PASS; GUI a/b/q, apps, hello, Rixie, restored shell |
| No i8042, text-only payload | BIOS and UEFI PASS as expected failure controls |
| No i8042, GUI payload then shell | BIOS and UEFI PASS as expected failure controls |

Host coverage includes class mismatch, absent functions, PCI header/BAR types,
disabled decoding, physical-width and 32-bit extent overflow, rejected ranges,
mapping failures, null/truncated mappings, truncated/invalid headers, offsets,
unsupported versions, 4,096 deterministic malformed-header cases and scan bounds.
The test interface exposes no configuration/MMIO write operation. Native GUI
exit reports kernel-CR3 reclamation before the shell; foreground hello/Rixie
execution and a fresh shell response prove normal restoration. No broader
historical campaign was rerun, and these checks do not establish physical USB
support or replace the retained N0–N6 evidence.

Genuine serial output (BIOS; UEFI uses BAR `0xc000000000`, command `0x7`):

```text
usb: xhci bus=0 dev=2 fn=0 vendor=0x1b36 device=0xd command=0x107 status=0x10 bar=0xfebf0000 bits=64
usb: inspected version=0x100 caplength=64 ports=8 slots=64 interrupters=16 dboff=0x2000 rtsoff=0x1000 hcs1=0x8001040 hcs2=0xf hcs3=0x0 hcc1=0x87001
usb: PCI command and BAR unchanged
usb: summary inspected=1 unsupported=0 invalid=0; bus-0 scan complete
```

Raw serial, QMP PCI snapshots, commands, host results and hashes are in the
[evidence index](usb1/README.md). The exact [changed-file inventory](usb1/changed-files.txt)
and [manifest](usb1/manifest.json) accompany the commit.

## Reproduction and personal preview

Use the separate USB checkout, not the interrupted D1 workspace. Install the
normal documented build prerequisites; supply an installed Rust compiler with
the `x86_64-unknown-none` target and installed or unpacked GRUB EFI modules:

```sh
cd ~/src/vos-usb
python3 -I -B scripts/test-usb-probe.py --host-only --output build/usb1-host-new
python3 -I -B scripts/test-usb-probe.py --output build/usb1-new \
  --rustc "$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc" \
  --grub-efi-dir /usr/lib/grub/x86_64-efi
```

Output paths must be new and below this checkout's `build/`. No root cleanup is
used. The harness creates local images and fresh OVMF variables, attaches no
host disks/network, bounds subprocesses and terminates only its own QEMU child.
It runs ten firmware/configuration cases, including expected failures. Historical
GRUB modules were used read-only through the explicit override on this machine;
ordinary builds need no private controller or evidence directory.

To preview the retained local BIOS image with xHCI present but input still routed
through QEMU's PS/2 device:

```sh
qemu-system-x86_64 -machine q35 -accel tcg -m 128M \
  -cdrom build/usb1-native-final/probe-bios.iso -boot d \
  -device qemu-xhci,id=xhci -nic none -serial stdio -no-reboot
```

For UEFI, change the ISO and add these drives (the variable template is protected
by snapshot mode):

```sh
qemu-system-x86_64 -machine q35 -accel tcg -m 128M \
  -cdrom build/usb1-native-final/probe-uefi.iso -boot d \
  -device qemu-xhci,id=xhci -nic none -serial stdio -no-reboot \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,snapshot=on,file=/usr/share/OVMF/OVMF_VARS_4M.fd
```

In the GUI, press `a`, `b`, then `q`; at `VOS>` try `apps`, `hello`, `rixie`,
`hello`, `exit`, and `echo restored`. No timed stress input is required. Adding
`-device usb-kbd,bus=xhci.0` reproduces the discovery configuration but may route
keyboard input to the unsupported USB device; do not expect it to control VOS.

## Preserved failures and next step

All original local outputs remain. Early harness failures were: an incorrect
console-test link recipe, missing hosted diagnostic I/O stubs, absent system
GRUB EFI modules, and a text-only negative fixture incorrectly supplying four
modules where the strict catalog requires three. These were harness/dependency
errors, not passing native results. Their raw output is retained in the index.
The first eight-case verifier is preserved exactly; the final verifier adds the
two separately exercised GUI negative controls and the bootstrap regression.

USB.2 should first establish bounded controller ownership/start/stop, validated
operational mappings, owned DMA/ring allocation and cleanup, then enumerate a
single root-port device and implement bounded HID keyboard transfers. Firmware
handoff, timeouts, reset/teardown, input-source integration and negative tests
must be designed explicitly. Do not grant raw USB/DMA access to applications.
This report does not authorize that next increment.

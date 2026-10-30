# USB survey v2.1: raw routing and EHCI values in the final SUMMARY

**Verified for PR review, not accepted or merged.** Presentation only. The read-only
[survey v2](usb-inc2-survey-v2.md) already captures the four Intel routing registers
and each EHCI's status and PORTSC words, but they were printed in the detail block
that scrolls off the physical console first. v2.1 repeats them in the final SUMMARY
block, which is printed last. **No new PCI or MMIO read, no write, no change to
controller behavior.** Verification is QEMU- and host-only; the survey has still
**not been run on the Dell** (that needs physical-test media and separate owner
approval, which this change does not have). QEMU has no Intel PCH xHCI, so the
routing values are exercised by hosted tests only.

Baseline: verified `main` `d67e3cdbf50ab7a67521fb5cc8ea81e2e0091a65` (PR #40), branch
`feat/usb-survey-v21-summary`. It is separate from the A1 ELF branch.

## What is added

| Line | Content |
| --- | --- |
| `usb: SUMMARY routing raw: xusb2pr 0x… xusb2prm 0x… pssen 0x… usb3prm 0x…` | The four registers at PCI config 0xd0-0xdc exactly as `intel_routing` captured them (about 100 columns). `usb: SUMMARY routing raw: not read` when they were not read (not an Intel xHCI, or the function reads all ones). A register that read zero prints `0x00000000`. |
| `usb: SUMMARY ehci <dev> cmd 0x… sts 0x… halted N configflag N ports N legacy bios N os N` (or `no-legacy`) | EHCI controller status. |
| `usb: SUMMARY ehci <dev> portsc: p1=xxxxxxxx …` | The captured 32-bit PORTSC words, six ports per line (at most three lines for 15 ports). `portsc: none` for a completed walk with zero ports. |
| `usb: SUMMARY ehci <dev> status NOT READ: <why> after N reads` | A walk that stopped. Registers after the stop are zeros only because the struct defaults to zero, so the values and port rows are suppressed instead of shown as zeros. |

Order inside the SUMMARY block: `usb2 routing`, `ehci connected root ports`, the new
routing line, the new EHCI lines, `dma`, and `DMA isolation NOT ESTABLISHED` last
(before the closing "survey complete" line). All previous SUMMARY markers, texts and
their relative order are unchanged; the banner is unchanged.

Code: `render_summary_routing` and `render_summary_ehci` in
[survey.rs](../../kernel/rust/usb/xhci/survey.rs), called from `summary_lines` in
[xhci_survey.rs](../../arch/x86_64/rust/xhci_survey.rs). They take only already
captured values (`&Option<Routing>`, `&UsbFn`, `&Ehci`) and an output sink, never a
`Pci` or `Mmio`, so they cannot read hardware. No hosted controller model, USB init
code or C file changed.

**Existing line not changed:** `SUMMARY ehci connected root ports: <dev>=N` still
prints `0` for an EHCI whose walk stopped, because its format is a compatibility
contract. The new `status NOT READ` line next to it is what tells the two apart.

## Verification

| Check | Result |
| --- | --- |
| Hosted survey gate (`scripts/test-usb-survey.py`, one existing mutant) | **PASS.** 62 tests (41 survey, 21 firmware; 5 survey tests are new), 4 compile-fail controls (no write on the Pci/Mmio/Phys traits), source scans. I did not run the full existing mutation campaign or add mutants. |
| Exact values and format | New tests assert whole lines: the routing line for typical, all-zero, extreme (`0xffffffff`, `0x80000001`) and not-read inputs; EHCI status for a typical controller, halted and cleared CONFIGFLAG, no legacy capability, zero ports. |
| Unavailable versus zero | A refusal at every read position of the EHCI walk, an all-ones first word and a bad CAPLENGTH each produce exactly the one `NOT READ` line with the real stop reason and read count, and no register value. |
| Extremes and truncation | 15 ports with every register all ones: status line plus three port rows, each whole and at most 118 columns (widest port row 105). Device id `ffff` renders. |
| Order and no-read | A source test pins the order of the markers above, that the isolation statement is last, and that `summary_lines` mentions no `inspect_`, `intel_routing(`, `read32`, `Mmio` or `Pci`. The existing source scans (no write path, routing offsets read once each) still pass. |
| Native QEMU, BIOS and UEFI | **PASS 18/18**: 14 survey cases (one xHCI, two xHCI behind a bridge plus EHCI, no USB, EHCI keyboard, Intel IOMMU, AMD IOMMU, tables above 1 GiB, each on both firmwares) with the new assertions, and 4 default-boot cases that still print no `usb:` line. The new assertions check one raw routing line, a last-line isolation statement, summary lines of at most 118 columns, and that the EHCI port words agree with the connected count. |
| Evidence-contract compatibility | The previous native script (before this change) was run unmodified against the Increment 2 survey ISOs: 14/14 baseline. Comparing the `usb:` lines of both runs case by case, the only differences are the added `SUMMARY routing raw` / `SUMMARY ehci` lines (27 lines over 14 cases) plus, in the 5 BIOS cases that boot a QEMU xHCI, the xHCI event-ring dequeue pointer `erdp`, which differs between runs. I attribute that to SeaBIOS having driven the controller before the survey (UEFI cases show no such difference), not to this change; I did not re-run to prove it. See [line-differential.txt](usb-survey-v21-summary/line-differential.txt). |
| Production image | Panic/fmt symbol count 0. `vos.bin` text grew from 331,776 to 335,872 B: the text segment crossed one page, so this is page-tiled; I did not measure the true symbol growth. |
| Screenshots | The native script takes a QEMU screenshot for the hold cases it lists; they are in the build tree and not committed. |

## Circular console limitation

The survey log is a circular buffer on the framebuffer console, and a physical
machine has no serial port: only the most recent rows stay on screen. Earlier
reports estimated roughly 57-67 rows at 1080p (the estimates differ and neither was
measured on the Dell), and the Dell output is expected to be longer than the
28-line QEMU output, so the top will scroll away. v2.1 does not change that; it
moves more of the facts the owner needs into the last lines. It does add lines to
that block: one for the routing registers and, per EHCI, one status line plus one
port line per six ports (about 5 lines for two six-port EHCIs). A very long run could still
push the earliest SUMMARY lines off the screen. The survey does not scroll back, so if
the block does not fit, the top of it is lost; if it fits, one photograph captures it.

## Limits

- Physical hardware is NOT TESTED. This does not show that any Dell register value will be
  read or what it will be.
- The Intel routing line is exercised by hosted tests only; QEMU's xHCI is not an Intel PCH.
- The unchanged `connected root ports` count can show 0 for an unread EHCI (see above).
- No physical media was created. Booting the Dell, changing firmware settings,
  controller handoff, halt, reset, port routing, enumeration and HID remain unauthorized.

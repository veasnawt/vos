# VOS Product Direction (Provisional)

| | |
|---|---|
| Status | **Provisional vision. Not a specification, not a schedule, not approved design.** Nothing described here exists yet unless it is explicitly listed under "What exists today". |
| Date | 2026-10-17 |
| Related | Kernel roadmap: `docs/plans/n2-foundation.md`. Native software architecture (proposal): `docs/architecture/native-software-platform.md`. |

This document records where VOS might go as a product, so that kernel decisions can be checked
against it. It deliberately stays separate from the technical roadmap: a direction here does not
create a commitment there.

---

## 1. What exists today

An experimental x86_64 kernel that boots under GRUB in QEMU, protects itself from user programs
and runs one separately built native program (`hello`) in ring 3 through a small, unstable
system-call ABI. There is no desktop, shell, input, storage, networking, multitasking,
package system or AI functionality. See the README and `docs/abi/syscall-v0.md`.

## 2. Direction

**VOS aims to be a friendly, comfortable, AI-era general-purpose operating system with an
exceptional creator-first experience**, built on its own kernel and platform.

- **An independent operating system.** VOS is not a Linux distribution and not a skin over
  another system. It has its own kernel, application model and system services.
- **A reusable kernel and platform.** The kernel, system services and SDK should be usable by
  others; third-party VOS-based distributions are a *future possibility*, not a current
  capability.
- **General-purpose.** Everyday computing should work well, not only creative work.
- **Creator-first.** Making things — software, media, documents — should feel native: fast
  tools, clear files and projects, good defaults, easy publishing.

## 3. Principles

1. **Human-centered usability.** Calm, comprehensible, accessible; the system explains itself
   and never surprises the user with hidden behavior.
2. **AI assistance is optional, transparent and permission-controlled.** It is off unless the
   user turns it on; it says what it is doing and with which data; it acts only through the same
   permissions as any other application; the user can review, undo and refuse its actions; the
   system remains fully usable without it.
3. **Security and privacy by design.** Least privilege (the capability-and-handle model in the
   architecture proposal), no ambient authority, local data stays local unless the user decides
   otherwise, and honest documentation of what is and is not protected.
4. **Native creator workflows.** Creation tools are first-class system citizens with stable
   APIs, not add-ons.
5. **General-purpose application compatibility.** Native applications first; compatibility with
   other platforms' software only through clearly bounded layers (architecture document,
   section 14), never at the cost of the native security model.
6. **Reusable foundations.** Components are designed so that distributions and other projects
   can build on them.

## 4. Proposed future components (proposals only)

| Name | Proposal | Status |
|---|---|---|
| `.vapp` | An application package format wrapping native ELF executables plus resources and a manifest (identity, version, requested permissions), signed, installed read-only. Relationship to the package proposal in the architecture document (section 13) to be decided. | Proposed name and idea only; no format exists |
| VOS Terminal | A native terminal application for the future text console and windowing system. | Planned user-space project; depends on input, a console or graphics stack, and process management |
| `vsh` | The VOS shell: a native command interpreter for VOS's own concepts (processes, handles, packages, permissions), with scripting. | Planned user-space project; depends on multitasking, IPC and storage |
| VOS Studio | A possible AI-native creation environment in which users describe software, edit it visually, inspect and test it, and publish it — always with full access to the generated source code, which remains the source of truth. | Long-term idea; no design yet |
| VOS-based distributions | Third parties building their own systems on the VOS kernel and platform. | Future possibility; needs stable ABIs, a licence decision and documentation first |

None of these is implemented, specified or scheduled. Names are working names.

## 5. What this means for the technical work

- Keep the kernel/user boundary small, documented and versioned (already the practice from ABI
  v0); creator tools and AI assistance both need a trustworthy permission model underneath.
- Do not build user-facing features before the foundations they need exist: VOS Terminal and
  `vsh` wait for input, multitasking and storage; `.vapp` waits for storage and a user-space
  loader; AI assistance waits for the permission model and services.
- Prefer designs that a distribution could reuse unchanged.

## 6. Open questions

1. Licence (blocks any reuse by others and the import of third-party code).
2. Whether `.vapp` replaces or refines the package proposal in the architecture document.
3. How AI assistance is hosted (on-device, user-chosen providers, both) and how its permissions
   are presented and audited.
4. Accessibility and internationalization requirements to design in from the first UI.
5. Which compatibility targets matter most for "general-purpose" use, and when.

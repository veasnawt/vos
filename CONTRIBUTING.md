# Contributing to VOS

Thank you for your interest in VOS. It is an experimental, independent x86-64 operating-system kernel, not a production OS. The goal is careful, reviewable progress backed by reproducible tests.

Before contributing, read [the README](README.md), [the current status](docs/STATUS.md), [the roadmap](docs/ROADMAP.md), and [the security policy](SECURITY.md).

## Ways to help

- Reproduce and improve QEMU tests, boot diagnostics, and documentation.
- Review kernel invariants, memory safety, user/kernel isolation, and device-driver assumptions.
- Propose small, well-scoped improvements with clear evidence.
- Report bugs with the tested commit, architecture, firmware emulator, QEMU command, expected behavior, and observed serial log.

For a significant change, open an issue or design discussion first. Avoid assuming a proposal in the roadmap is already approved or implemented.

## Development and testing

Build on Ubuntu/Debian or WSL2 with the prerequisites in [docs/HANDOFF.md](docs/HANDOFF.md).

```sh
make                 # x86_64 kernel (default)
make iso             # BIOS/GRUB ISO
make test-all        # BIOS regression tests, including i386
make test-uefi       # UEFI/OVMF regression tests
```

The UEFI test targets require extra GRUB EFI files, mtools, and OVMF. Do not run `make clean` on a workspace whose generated artifacts you need to keep. Tests can take time and may require QEMU setup.

For changes affecting the kernel, ABI, boot, memory, interrupts, filesystem, or build tools, explain what you tested. State `NOT TESTED` where applicable; do not represent QEMU coverage as physical-hardware verification. Include failure or panic expectations for negative tests.

## Pull requests

1. Make one focused change per pull request; describe the motivation, design impact, and rollback.
2. Keep C and assembly freestanding and consistent with surrounding code. Avoid introducing a host operating-system or C-library runtime dependency.
3. Preserve existing ABI expectations unless the change includes an explicitly reviewed compatibility plan.
4. Update tests and documentation when changing behavior.
5. Do not add generated build output, credentials, signed binaries, private keys, customer data, or large vendor assets.
6. Identify third-party code, fonts, fixtures, or generated material, including its source, authorship, license, and the right to redistribute it.
7. Prefer commits signed off with `git commit -s`, following the [Developer Certificate of Origin 1.1](https://developercertificate.org/). A sign-off certifies your right to submit the contribution; it is not a copyright assignment.

Maintainers may ask for changes and are not obliged to merge a contribution. Community proposals are welcome, while final decisions on this repository's official direction remain with its maintainers.

## Licensing and attribution

VOS is offered under [GNU GPL version 3 only](LICENSE) (SPDX: `GPL-3.0-only`). Contributions intended for inclusion should be compatible with that license and should preserve applicable copyright and license notices. Contributors retain their copyrights; there is no blanket copyright assignment.

A GPL license does not prevent private modifications. Distribution triggers obligations under the applicable license terms.

## Security and hardware safety

For potentially exploitable flaws, follow [SECURITY.md](SECURITY.md) rather than posting an attack recipe or credentials in a public issue.

Do not ask others to flash raw images onto their primary disks, change boot firmware, or run unverified code on real hardware. The reference environment is QEMU. Physical testing requires an explicit safety plan and backups.

## Conduct

All contributors must follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

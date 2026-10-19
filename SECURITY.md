# Security Policy

## Project status

VOS is a research-stage kernel. **It is not a hardened, general-purpose, or production-ready operating system.** QEMU/OVMF/SeaBIOS coverage is documented in [docs/STATUS.md](docs/STATUS.md). Physical hardware, Secure Boot, and broad device support are not verified at this checkpoint.

Do not use VOS to protect confidential information, handle sensitive workloads, or boot from a disk containing important data.

## Reporting a vulnerability

Please **do not publish an exploitable security report, credentials, or a destructive proof of concept in a public GitHub issue**.

If the repository's **Report a vulnerability** feature is enabled, use [GitHub private vulnerability reporting](https://github.com/veasnawt/vos/security/advisories/new). If that option is unavailable, contact the maintainer through a private contact method listed on the [maintainer's GitHub profile](https://github.com/veasnawt) and request a secure reporting channel before sending sensitive details.

Provide, when safe:

- Affected commit or checkpoint tag and the relevant component.
- QEMU/OVMF/SeaBIOS versions, CPU/machine settings, and steps to reproduce.
- The security boundary involved and expected versus actual behavior.
- A minimal non-destructive demonstration and the relevant logs, redacted of secrets.

Do not send private keys, passwords, real user data, or unredacted crash dumps.

## Response and disclosure

This is a small experimental project. There is no guaranteed security-support SLA or fixed patch timeline. Maintainers will review reports as capacity permits and coordinate disclosure when appropriate. A report's receipt does not imply the flaw is confirmed or fixed.

## Scope and limitations

The security-relevant surfaces include the kernel, the boot handoff, system calls, ELF loading, user isolation, memory mapping, IPC, and device/filesystem code. The project explicitly does **not** promise production security, verified operation on physical computers, safe disk installation, or third-party application compatibility.

For ordinary non-sensitive bugs and feature requests, use GitHub Issues instead.

See [CONTRIBUTING.md](CONTRIBUTING.md) for safe test and contribution practices.

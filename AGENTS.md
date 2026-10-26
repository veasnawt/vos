# Agent guidance for VOS

These instructions guide compatible coding agents; they are not a security
boundary. CI, repository rules, reviewed authorizations and independent checks
remain necessary.

## Read first

Read [README](README.md), [STATUS](docs/STATUS.md), [ROADMAP](docs/ROADMAP.md),
[CONTRIBUTING](CONTRIBUTING.md) and [SECURITY](SECURITY.md). Follow the
[publication documentation checklist](docs/milestone-publication-checklist.md)
when publication is authorized. Link to these policies rather than copying them.

VOS is an independent OS targeting physical x86_64 computers. Current verification
is QEMU-based, not proof of physical-hardware readiness or production security.
N0-N5 and the documented N6.1-N6.3 components are accepted; the full N6 desktop
remains incomplete. The kernel is currently primarily C/assembly. Native
applications are Rust-first; kernel migration and broader hardware portability
are proposed tracks, not implemented features or permission to start them.

## Changes and security

- Keep changes small, reviewable and inside the authorized source scope.
  A roadmap proposal or configuration file does not authorize implementation.
- Preserve kernel behavior, documented ABI compatibility, capability restrictions,
  memory isolation, DMA ownership, bounded resources and explicit cleanup.
- Validate untrusted device, network, file and executable input. Review unsafe
  Rust, FFI and assembly boundaries. Never bypass safety for performance.
- Ask for human authorization before destructive actions, physical-disk or
  firmware operations, evidence retirement, expanding maintainer limits, or
  actions outside the approved task. Do not reset, rebase or force-push existing
  shared history, remove worktrees, discard stashes, or merge without authorization.
- Do not modify immutable evidence or relabel failed/interrupted runs as passing.
  Preserve unfinished records and disclose monitoring gaps.
- Root make clean deliberately fails closed: build/ may contain protected
  evidence and controller dependencies. Do not replace that protection with
  recursive deletion. Selective cleanup needs a separately reviewed design.

## Verification and contributions

Use focused checks for the affected component. Reuse earlier evidence only when
source/input identity and coverage justify it. Report failures, missing coverage
and NOT TESTED conditions accurately; do not hide failures with automatic retries.
Keep release-performance measurements separate from sanitizers/debug tests.
Capture genuine running-VOS screenshots when they show meaningful behavior;
identify mockups as design references.

Follow CONTRIBUTING for build prerequisites, PR scope, licensing, attribution and
sign-off guidance. Never commit secrets, generated build trees or oversized
archives. Publication and merging require the applicable human authorization.
Inspect Build smoke, Secret history audit and other applicable CI checks;
report pending, failed and unavailable checks honestly.

## Maintainer-local workflows versus contributor workflows

Maintainer sessions may additionally require task-specific isolation, resource
admission, preservation inventories and receipts. Respect those requirements
when supplied; do not invent or waive them. Storage cleanup, backup and relocation
are separate actions requiring their own authorization.

External contributors are not required to possess private maintainer evidence,
budgets, machine identifiers, WSL paths or historical controller directories.
Use the documented public build/test interfaces and disposable fixtures.
The versioned controller foundation is optional tooling, not an automatic
replacement for established maintainer guards or an OS sandbox.

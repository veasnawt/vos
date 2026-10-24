# N4 closure: interactive console and native Rust applications

The original N4 objectives are complete and published on
`feat/n4-interactive-console`, ready for integration. The owner handles the
GitHub pull request and merge. Main remains `d9a4ade0db1df8ab7c8c7fe3c4553cd3f36c3622`
(the integrated N3 baseline); no N4 merge, history rewrite or force-push occurred.

## Published implementation

| Increment | Commit | Files and technical record |
| --- | --- | --- |
| Keyboard to console | [3a2b642](https://github.com/veasnawt/vos/commit/3a2b642cd3f2e7b719135c86e77cc787a865b327) | [First input](n4-first-input.md) |
| Buffered command input | [87b99df](https://github.com/veasnawt/vos/commit/87b99dfc867c45bd01f512cf9ecac0117d1a6eac) | [Buffered input](n4-buffered-input.md) |
| Native Rust vsh and console ABI | [1beb406](https://github.com/veasnawt/vos/commit/1beb40677bfeb605da5b2920b70af0fc24138493) | [Native shell](n4-native-vsh.md) |
| Reusable Rust SDK and independent app | [f7ac434](https://github.com/veasnawt/vos/commit/f7ac43478ea548ddc4bb74ae177b0bb828e67596) | [Exact 34-file inventory](n4-sdk/changed-files.txt), [guide](../rust-apps.md), [results](n4-rust-foundations.md) |
| Normalized working-directory prompt | [bdde1bc](https://github.com/veasnawt/vos/commit/bdde1bc33f99030ba51fa78078a6db90f87f2a07) | [Exact 16-file inventory](n4-prompt/changed-files.txt), [focused results](n4-dynamic-prompt.md) |

Keyboard input, editable bounded lines, the interactive console, native ring-3
`vsh`, all seven requested commands and reusable Rust application foundations are
delivered. File commands use real read-only ext2 through capability-scoped ABI v2.
The shell owns no second keyboard driver and command parsing remains in userspace.

The prompt uses the shell's actual normalized cwd: `VOS>`, `VOS>docs>`,
`VOS>docs>results>`. Successful parent/root navigation updates it; failed `cd`
preserves it. The existing input bounds and Backspace boundary remain intact.

## Verification and reproducibility

The SDK publication retains its accepted host contracts, empty-output-path guards,
native ELF/loader checks, shared-shell BIOS/UEFI interactions and independent
example BIOS/UEFI execution. The example uses the shared no_std entry/runtime
and existing ABI v3 without importing shell internals or receiving console/file
authority. See the [Rust application guide](../rust-apps.md) for build commands,
compiler target, runtime conventions, bindings and capability restrictions.

The prompt change passes three host tests, strict shell ELF and loader validators,
and 13 exact prompt cases on each firmware. Both firmware interactions also check
the seven existing commands, real files, unchanged read-only disk, ABI v0 hello,
status-0 shell exit, frame reclamation and halt. No N3 campaign was repeated.

The [SDK evidence](n4-sdk/evidence.json) references the 302-entry local manifest
SHA-256 `c8958cb9a9f914d98fc4126ff6532300280173b236ba6dccc3a4e83460db55be`.
The [prompt evidence](n4-prompt/evidence.json) records its 157-entry local manifest,
source/product identities, raw commands/transcripts and clean guard outcome.
The [closure accounting and integrity record](n4-closure.json) records final
read-only preservation checks, publications and the storage snapshot.

Raw records describe their original checkpoints; fields such as SDK_committed
false in pre-publication evidence are preserved, not rewritten. Publication
commits and this closure record supply the later status.

## Authentic screenshots

- [BIOS independent Rust application](../images/n4/vos-n4-bios-rust-sdk.png)
- [UEFI independent Rust application](../images/n4/vos-n4-uefi-rust-sdk.png)
- [BIOS nested cwd prompt and rejected cd](../images/n4/vos-n4-bios-cwd-prompt.png)
- [UEFI nested cwd prompt and rejected cd](../images/n4/vos-n4-uefi-cwd-prompt.png)

The captures are lossless conversions of actual QEMU framebuffer dumps. Original
dumps, additional navigation captures and raw serial transcripts remain in their
local evidence roots. No generated or composited UI depicts unimplemented behavior.

## Preservation and limits

All prior N3 evidence and failed/interrupted attempts remain intact, including
the unfinished, unsealed and unarchived D1r3 root. Seven protected binary identities,
the 38,130-entry preservation snapshot, N3 publication manifest/archive and previous
N4 artifact maps are reverified. All 13 worktrees and two stashes are preserved.
No protected outputs were rebuilt, removed or replaced.

The existing isolation detection, 90-second monitor queries, failure conditions,
host reserves and 3360 MiB ceiling remain unchanged. Failed launch roots and manual
test outputs are counted; evidence was not deleted to create room.

The pre-publication storage snapshot counts 404.98 MiB of N4 attempts and manual
outputs. Conservative combined accounting is 2786.18 MiB of 3360 MiB, leaving
573.82 MiB, including a 2 MiB provision for publication metadata. The linked
closure record preserves exact byte/MiB values; C:, E: and Linux reserves pass.

Current limits remain QEMU-only hardware verification, US ASCII, 128-byte input,
one granted console owner, fixed-size buffers/no allocator, and trusted read-only
files. Quoting, pipes, writable storage, networking, GUI, package management and
Rixie Core are outside the agreed N4 scope. They are not integration prerequisites.

The original N4 scope is complete. Development stops after this publication;
the owner can proceed with the pull request and merge.

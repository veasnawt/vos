# VLE-1: a lower-cost ledger format (research, not active)

This directory is the handoff package for an **independent** VLE-1 verifier and the public part of the ledger-format research.
The VOS R-series ledger keeps its current format and procedure; nothing here changes it.

| File | Purpose |
| --- | --- |
| `SPEC.md` | the normative format, version 1 |
| `THREAT_MODEL.md` | threats, residual risks and the verifier's acceptance criteria |
| `CODEX_TASK.md` | the task given to the independent implementer |
| `CUTOVER.md` | a proposed, unauthorized cutover procedure with rollback guarantees |
| `fixtures/` | a synthetic conformance log (no real ledger data), states, test keys, encoding inputs and `VECTORS.json` |
| `run_vectors.py` | the language-neutral conformance runner |

## Status

* **Independent verifier: PENDING.** It is to be written by Codex in Go from this directory alone. Codex could not be invoked: the
  Codex CLI on the maintainer machine was not logged in, and logging in is the owner's action. No independent verification is
  claimed.
* Reference writer and a verifier by the same author exist in the maintainer's private research directory; they are deliberately
  not published here, so that they cannot serve as a template.
* That same-author verifier passes all 71 vectors. On private copies of the real ledger history (revisions 5-83) the writer
  reproduces every state byte for byte and both replays (from genesis and from the latest checkpoint) reach the live state hash.

## Running the independent implementation (owner)

```
codex login                                   # the owner's own account
cp -r <this directory> /tmp/vle1-handoff      # a copy containing nothing else
cd /tmp/vle1-handoff
codex exec --skip-git-repo-check --sandbox workspace-write --json -C /tmp/vle1-handoff "$(cat CODEX_TASK.md)" > codex-events.jsonl
python3 run_vectors.py . -- verifier/vle1verify
```

`codex-events.jsonl` records every command Codex ran; it must show no access outside the copy. The sandbox restricts writes, not
reads, so this audit is part of the independence evidence.

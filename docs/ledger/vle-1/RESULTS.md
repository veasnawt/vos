# VLE-1 research results (2026-11-16)

Research evidence only. The live ledger was not changed; every experiment ran on isolated, read-only copies.

| Check | Result |
| --- | --- |
| Independent verifier (Codex, Go) | **PENDING**: Codex was not logged in on the maintainer machine; nothing independent is claimed |
| Same-author verifier (Python) on the 71 conformance vectors | 71 / 71 |
| Real history (revisions 5-83): states rebuilt by the spec encoding | 79 / 79 byte-identical |
| Replay from genesis / from the latest checkpoint | PASS / PASS, both ending at the live revision-83 state hash |
| Faults injected into the real-history log | 11 / 11 rejected with the expected code; the no-key truncation control passes, as specified |
| T83 (revision 82 -> 83) | **retrospective** verification: committed before the tools existed |
| Concurrent shadow verification of a real transition | none yet |

**Cost of a transition** (guarded bytes; current = recorded charges; VLE-1 = event + authorization record + growth of the one
state file, plus a full checkpoint when one falls due):

| | checkpoint every 8 | every 16 | every 32 |
| --- | --- | --- | --- |
| Revisions 49-83 (35 transitions) | -80.6% | -86.6% | -88.5% |
| Forecast for the next transition | -79.9% | -86.1% | -89.1% |

Without a checkpoint, each of the last eight transitions would have cost 12,195-39,565 guarded against 240,750-304,403 today.

History facts found (unchanged, not reconstructed): no snapshots for revisions 2-4; 11 early events carry no ledger hash; the
strict event chain is continuous from revision 44; 9 transitions have no linking event.

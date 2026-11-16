# VLE-1 threat model and acceptance criteria

## Assets

The authoritative accounting state of the VOS R-series ledger; the integrity of its history (every transition, its order and its
authorization); and the owner's ability to detect any change made without authorization.

## Adversaries and failures

| Threat | Example | Detection in VLE-1 | Vectors |
| --- | --- | --- | --- |
| Accidental corruption | bit rot, a torn write, a bad copy | hashes of every file; canonical encodings | F08, F15, F17, F18 |
| Interrupted transition | power loss between the event and the state write | `recover` (ROLL_FORWARD_STATE / _HEAD / STOP); `.tmp` files never trusted | R01-R08, V04 |
| Writer defect | wrong delta, key order lost, number token rewritten | replay must rebuild the exact state hash | F09, F13, F14 |
| Edited history (no key) | an event changed, reordered, removed, replayed, added | hash chain, sequence numbers, exact file set | F04-F07, F10-F12, F26, F27 |
| Malformed authorization link | hash not in canonical form, record missing or altered | event schema; content-addressed authorization records | F23-F25 |
| Undefined input | duplicate members, lone surrogates, bad number tokens, invalid UTF-8 | strict parser | F35-F37, C-x01..x11 |
| Consistent truncation | newest events deleted and HEAD rewritten to match | **only** the owner-signed anchor | F02 (control: undetected), F03 |
| Whole-log rewrite by someone with write access | every file replaced consistently | **only** the anchor, and the witness copy | F29, F30 |
| Forged anchor | signature by another key, broken chain | Ed25519 with a key supplied out of band | F28-F30, F38 |

## Out of scope / residual risks

* An attacker who holds the owner's private key, or who can replace both the log and the verifier's copy of the public key, is not
  detected. The key must stay with the owner; the public key must come from a source the attacker cannot write.
* The witness (an independently retained copy of each anchor statement) is outside the verifier; its protection is operational.
* Availability (deletion of the whole log and its witness) is not addressed by the format.
* The verifier itself must be trustworthy: hence an independently written implementation, compared against these vectors and
  against the reference writer's output, before any activation.

## Acceptance criteria for an independent verifier

1. Written from `SPEC.md`, this file and the fixtures only, without access to the reference writer or the reference verifier.
2. Standard library only; builds offline; one command implementing section 15 of the specification.
3. `run_vectors.py <dir> -- <verifier>` reports every vector passing (exit 0).
4. On the owner's real history (run by the maintainer, data not public): replay from genesis and from the latest checkpoint PASS
   with the same head hashes as the reference verifier; the owner's live state hash is reproduced.
5. Every disagreement with the reference implementation, the vectors or the specification is reported, not resolved silently. A
   disagreement blocks activation (it does not block unrelated kernel work).
6. The implementer lists every point where the specification was ambiguous and how it was read.

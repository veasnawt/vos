# Proposed VLE-1 cutover procedure (not authorized)

This is a proposal for the owner's review. Nothing here is active; each stage needs its own authorization, and the current ledger
remains the only accounting authority until stage 5.

## Preconditions (all required)

1. An independently written verifier (different author, Go, standard library only) passes every conformance vector, and on the
   owner's real history reproduces the reference results exactly (head hashes from genesis and from the latest checkpoint).
2. At least two **concurrent** shadow verifications of real transitions (section "Evidence classes" below) with zero mismatches,
   each with the tool hashes fixed before the transition.
3. Owner decisions recorded in an admitted transition: the accounting amendment (charge what is retained), the checkpoint interval,
   the anchor `key_id`, the public key's distribution and the witness location; hash-pinned approval of the writer and verifier.
4. The owner has generated the production Ed25519 key on a device under the owner's exclusive control. The private key is never
   given to the development assistant, a worker, a repository or a connected service.

## Stages

| Stage | Action | Authority | Reversible by |
| --- | --- | --- | --- |
| 1 | Parallel shadow: after every current-format transition, rebuild a VLE-1 log from isolated copies and verify it | current ledger | doing nothing |
| 2 | Owner decisions and tool approval (preconditions 3-4) | current ledger | not adopting |
| 3 | Dry cutover on copies, including interrupted-write drills at each write step and a rollback drill | current ledger | discarding the copies |
| 4 | Cutover transition, written under the **current** rules: the last full snapshot is kept as usual and becomes the VLE-1 genesis; the transition records the tool hashes, the genesis hash and the public key | current ledger | stage 6 |
| 5 | First VLE-1 transition: event, state, checkpoint if due, head, then the owner's signed anchor and its witness copy; verifier replay before and after | VLE-1 | stage 6 |
| 6 | Rollback, if ever needed: resume the current procedure from the current state file (same encoding; no conversion); the VLE-1 log is kept as evidence and never edited | current ledger | — |

## Rollback guarantees

* The current state file keeps the current encoding at every step, so the current procedure can resume at any time without a
  conversion.
* Nothing is deleted or rewritten: not the existing snapshots, events, seals, receipts, STOP records or SEC-02 evidence, and not
  the VLE-1 events or checkpoints. A wrong transition is corrected by a later one.
* Every interrupted write is classified by `recover` as CONSISTENT, ROLL_FORWARD_STATE, ROLL_FORWARD_HEAD or STOP; a STOP goes to
  the owner and is never resolved automatically.
* The genesis is a byte-for-byte copy of an authoritative snapshot; earlier gaps (missing snapshots 2-4, events without ledger
  hashes) stay documented and are never reconstructed.

## Evidence classes (not interchangeable)

* **Historical replay**: the existing snapshots rebuilt through VLE-1 after the fact. Research evidence only.
* **Retrospective verification of a real transition**: a transition that was committed before the tools existed, replayed later.
* **Concurrent shadow verification of a real transition**: tools whose hashes were fixed before the transition, run on isolated
  copies immediately after it and before the next one. Only this class counts toward precondition 2.

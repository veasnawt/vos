# Task for the independent implementer (Codex)

Implement a **VLE-1 verifier in Go, standard library only** (`crypto/sha256`, `crypto/ed25519`, ...; no third-party modules, no
network). Work only from the files in this directory:

* `SPEC.md`: the normative format. Section 15 defines the command line and output.
* `THREAT_MODEL.md`: threats and the acceptance criteria.
* `fixtures/`: the conformance log, states, keys (test keys only), encoding inputs and `VECTORS.json` (expected results).
* `run_vectors.py`: the conformance runner.

Rules:

1. Do not read, search for or reuse any other VLE-1 implementation, including anything outside this directory. Write the parser,
   both serializers, the operations, the replay, recovery and anchor checks yourself from the specification.
2. Do not use `encoding/json` to decode ledger data: it loses member order and number tokens. A hand-written parser is expected.
3. Do not change `SPEC.md`, the fixtures or the vectors. If you believe one of them is wrong, say so in `NOTES.md` and keep your
   implementation faithful to your reading of the specification.
4. Build: `go build -o vle1verify .` in a new directory `verifier/`. Run: `python3 run_vectors.py . -- verifier/vle1verify`.
5. Report in `verifier/NOTES.md`: the conformance result (copy the runner's summary line), every vector that fails, every
   ambiguity you found in the specification and how you read it, and the commands you ran.

Done means: the runner passes every vector, or `NOTES.md` explains each failure. Independence is more important than passing:
do not adjust the code to match a vector you believe contradicts the specification.

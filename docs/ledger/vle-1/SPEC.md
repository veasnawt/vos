# VLE-1: VOS ledger event-log format, version 1

**Status: proposal, non-normative for the live ledger.** This document is normative for implementations of VLE-1 (writers and
verifiers). It does not change the authoritative VOS ledger, which keeps its current format and procedure until a separately
authorized cutover. The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

## 1. Purpose and model

The current ledger keeps, per transition, a complete copy of the previous state. VLE-1 keeps one current state file and, per
transition, one immutable **event** describing the change, chained by SHA-256 hashes, plus periodic full **checkpoints** and a
**head record**. A verifier rebuilds every state from a checkpoint and the events and compares exact hashes. An external
**anchor**, signed with an Ed25519 key held only by the owner, authenticates the newest head.

Terms: a **revision** is a non-negative integer naming a state; event *r* turns revision *r-1* into revision *r*. The
**genesis** is the first revision the log covers. **Hash** always means the lowercase hexadecimal SHA-256 (64 characters) of the
exact bytes of a file.

## 2. JSON value model

Every file is UTF-8 JSON (RFC 8259) with these additional rules. A conforming parser MUST reject (code `E_JSON`) any input that
violates them.

1. **Objects are ordered.** An object is a sequence of members; member order is part of the value. Duplicate member names in one
   object MUST be rejected.
2. **Numbers are tokens.** A number is the exact sequence of characters that matched the JSON `number` grammar. Implementations
   MUST NOT convert it to a machine number and back: `1`, `1.0`, `1e5`, `100000`, `-0` and `0` are six different values. A token
   matching the grammar of RFC 8259 is accepted; `01`, `1.`, `.5`, `+1`, `NaN`, `Infinity` are rejected.
3. **Strings** are sequences of Unicode scalar values after decoding escapes. A `\uXXXX` escape of a high surrogate MUST be
   immediately followed by a `\uXXXX` escape of a low surrogate (the pair decodes to one scalar value); any other use of a
   surrogate escape MUST be rejected. Raw (unescaped) characters below U+0020 inside a string MUST be rejected. Invalid UTF-8 MUST
   be rejected.
4. **Literals** are `true`, `false`, `null`.
5. Whitespace (space, tab, LF, CR) is permitted between tokens on input; outputs are defined exactly in sections 3 and 4.
6. A file holds exactly one value, followed only by whitespace.

## 3. State encoding (exact bytes)

The bytes of a state file are `S(root, 0) + eof`, where `eof` is either the empty string or a single LF (`"\n"`). The value of
`eof` is recorded with every state (section 5). `S(v, d)` at nesting depth `d`:

* `null`, `true`, `false`: the literal.
* number: the token, unchanged.
* string: `E(s)` (below).
* empty array: `[]`. Non-empty array: `[`, LF, then the items, each as `I(d+1)` + `S(item, d+1)`, separated by `,` LF; then LF,
  `I(d)`, `]`.
* empty object: `{}`. Non-empty object: `{`, LF, then the members in order, each as `I(d+1)` + `E(name)` + `: ` (colon, one
  space) + `S(value, d+1)`, separated by `,` LF; then LF, `I(d)`, `}`.

`I(n)` is 2 x *n* spaces. No other whitespace appears.

`E(s)` is `"`, the escaped characters, `"`. Each scalar value *c* of *s* is written as:

| *c* | written as |
| --- | --- |
| `"` | `\"` |
| `\` | `\\` |
| U+0008, U+000C, U+000A, U+000D, U+0009 | `\b`, `\f`, `\n`, `\r`, `\t` |
| other U+0000 - U+001F, and U+007F and above, up to U+FFFF | `\u` + 4 lowercase hex digits |
| above U+FFFF | its UTF-16 surrogate pair, each as `\u` + 4 lowercase hex digits |
| any other (U+0020 - U+007E) | the character itself |

Consequently every encoded file is pure ASCII. (This is the output of Python's `json.dumps(value, indent=2)` with default
options, which produced every existing VOS ledger state; VLE-1 defines it independently of any library.)

## 4. Record encoding (compact)

Event, checkpoint-metadata, head and anchor records are one line: `C(value)` followed by one LF. `C(v)`:

* literals and numbers as in section 3; strings as `E(s)`;
* arrays: `[` + items joined by `,` + `]`; objects: `{` + `E(name)` `:` `C(value)` members joined by `,` + `}`.

No whitespace appears anywhere in `C(v)`. A record file MUST be exactly `C(parse(file)) + LF`; otherwise it is rejected with
`E_ENCODING` (if it parses) or `E_JSON` (if it does not). Values copied from states into records (delta values) keep their member
order and number tokens.

An **integer field** in a record is a number token matching `0|[1-9][0-9]*`. A **hash field** is a string of exactly 64
characters from `0-9a-f`.

## 5. Events

The file `events/NNNNNN.json` (NNNNNN = `to_revision`, six digits, zero-padded) holds one event: an object with exactly these
members, **in this order**:

| member | type | meaning |
| --- | --- | --- |
| `format` | string | `"vos-ledger-event-1"` |
| `seq` | integer | `to_revision - genesis_revision - 1` |
| `from_revision` | integer | *r-1* |
| `to_revision` | integer | *r* |
| `prev_event_sha256` | hash | hash of event *r-1*'s file; for the first event, the genesis hash |
| `prev_state_sha256` | hash | hash of state *r-1* |
| `new_state_sha256` | hash | hash of state *r* |
| `eof` | string | `""` or `"\n"`: the terminator of state *r* |
| `type` | string or null | transition type (informative) |
| `utc` | string or null | time (informative) |
| `authorization_sha256` | hash or null | hash of the authorization record of this transition |
| `v1_event` | null or object | cross-reference to an event of the current ledger: exactly `{"name": string, "sha256": hash}` |
| `delta` | array | operations (section 6), applied in order |

A record whose members differ in name, order, number or type from this table is rejected with `E_EVENT`.

## 6. Operations

An operation is a three-element array `[op, path, value]`. `op` is the string `"set"` or `"append"`; `path` is an array whose
elements are strings (object member names) or integers (array indices). Any other shape, or another `op`, is `E_OP`.

To **resolve** a path prefix, start at the root; for each element, the current value MUST be an object containing that member
(element is a string) or an array with more items than the index (element is an integer). Otherwise: `E_PATH`.

* `["set", [], v]` replaces the root with `v`.
* `["set", [p..., k], v]`: resolve `[p...]`; if it is an object and `k` is a string, then if a member `k` exists its value is
  replaced **in place** (its position does not change), else the member `k: v` is added **at the end**. If it is an array and
  `k` is an integer smaller than its length, item `k` is replaced. Anything else: `E_PATH`.
* `["append", [p...], v]`: resolve `[p...]` (the empty path is the root); it MUST be an array (else `E_PATH`); `v` is added at its
  end.

There is no delete or move. A writer expresses the removal or reordering of members by `set` of the smallest enclosing value.

## 7. Layout of a log directory

```
HEAD.json                    head record (section 8)
events/NNNNNN.json           one per revision genesis+1 .. head
checkpoints/NNNNNN.json      full state at revision NNNNNN (genesis always)
checkpoints/NNNNNN.meta.json checkpoint metadata (every checkpoint except genesis)
authorizations/<hash>        authorization records, named by their own hash
anchors/NNNNNN.anchor.json   signed anchor statements (section 10)
anchors/NNNNNN.sig           their signatures
```

Files whose name starts with `.` and ends with `.tmp` are interrupted writes: verifiers MUST ignore them (they are never
trusted) and MAY report them. Any other unexpected file in `events/` is an error (`E_FILES`).

## 8. Head record

`HEAD.json` is one record with exactly these members, in this order: `format` (`"vos-ledger-head-1"`), `genesis_revision`
(integer), `genesis_sha256` (hash), `head_revision` (integer, >= genesis), `head_event_sha256` (hash; equal to `genesis_sha256`
when there is no event), `head_state_sha256` (hash), `events` (integer, `head_revision - genesis_revision`),
`checkpoint_interval` (integer, >= 1). Violations: `E_HEAD`.

## 9. Checkpoints

The genesis checkpoint is the byte-for-byte copy of the genesis state. A checkpoint is written at every revision *r* with
`(r - genesis_revision) mod checkpoint_interval = 0`, together with its metadata record: exactly `revision` (integer),
`state_sha256` (hash), `event_sha256` (the hash of `events/<r>.json`), in this order. Checkpoints and events are never deleted
or rewritten by VLE-1; their retention is permanent in this version.

## 10. Anchors (external authentication)

An anchor statement is one record with exactly these members, in this order: `format` (`"vos-ledger-anchor-1"`), `key_id`
(string), `head_revision` (integer), `head_event_sha256` (hash), `head_state_sha256` (hash), `predecessor_anchor_sha256` (hash
of the previous anchor statement file, or null for the first). It is stored as `anchors/NNNNNN.anchor.json` (NNNNNN =
`head_revision`). Its signature is Ed25519 (RFC 8032, pure, no prehash) over the exact bytes of that file, stored in
`anchors/NNNNNN.sig` as 128 lowercase hexadecimal characters and one LF.

The public key is 32 bytes, given to the verifier **out of band** as a file of 64 lowercase hexadecimal characters and one LF. A
verifier MUST NOT take the key from the log. The private key is held only by the owner; VLE-1 never stores it.

Anchors are verified only when the verifier is given a key. Then, in revision order: every statement MUST be a valid record
(`E_ANCHOR_CHAIN` otherwise), its predecessor hash MUST be the hash of the previous statement file (null for the first)
(`E_ANCHOR_CHAIN`), its signature MUST verify (`E_ANCHOR_SIG`); and the **newest** anchor MUST name the same head revision,
head event hash and head state hash as the replay result (`E_ANCHOR_HEAD`; also when there is no anchor at all).

A copy of each anchor statement SHOULD also be kept by an independent **witness** outside the log's storage (for example a
signed tag or a record held by the owner). An unsigned hash stored beside the log authenticates nothing.

## 11. Verification (replay)

A verifier replays a log in this order and stops at the **first** violation, reporting its code and the revision at which it
was found (or null).

1. `HEAD.json`: section 8 (`E_HEAD`).
2. File set: `events/` holds exactly `NNNNNN.json` for every revision from genesis+1 to `head_revision`, apart from ignored
   `.tmp` files (`E_FILES`, revision null).
3. Genesis: `checkpoints/<genesis>.json` exists, its hash equals `genesis_sha256`, and it is `S(parse(file), 0) + eof` for its
   own trailing terminator (`E_GENESIS`).
4. Start: the genesis, or, when asked to start from the latest checkpoint, the highest revision with a metadata record, which
   MUST be valid with `revision` equal to its name, its checkpoint file's hash equal to `state_sha256`, and `event_sha256` equal
   to the hash of `events/<revision>.json` (`E_CHECKPOINT`). The chain value is the genesis hash or that event hash.
5. For each event *r* after the start, in order:
   1. the file parses (`E_JSON`) and is canonical (`E_ENCODING`);
   2. section 5 (`E_EVENT`);
   3. `seq`, `from_revision` and `to_revision` match *r* (`E_SEQUENCE`);
   4. `prev_event_sha256` equals the chain value (`E_PREV_EVENT`);
   5. `prev_state_sha256` equals the hash of the current state (`E_PREV_STATE`);
   6. the operations apply (`E_OP`, `E_PATH`);
   7. the hash of `S(state, 0) + eof` equals `new_state_sha256` (`E_REPLAY`);
   8. if `authorization_sha256` is not null, `authorizations/<hash>` exists and its hash equals the name (`E_AUTH`);
   9. if `checkpoints/<r>.json` exists, its hash equals the new state hash; if its metadata exists, it is valid with matching
      revision, state hash and event hash (`E_CHECKPOINT`);
   10. the chain value becomes the hash of the event file.
6. The chain value equals `head_event_sha256` and the state hash equals `head_state_sha256` (`E_HEAD_MISMATCH`).
7. With a key: section 10.

A **PASS** reports `head_revision`, `head_event_sha256`, `head_state_sha256` and the number of events replayed.

**Limit.** A log truncated at its tail *together with* a consistently rewritten `HEAD.json` replays cleanly. Only the signed
anchor (and its witness) detects it. Without a key, a verifier MUST NOT be described as detecting truncation.

## 12. Writing a transition and interrupted writes

A writer performs, in order, each file written under a temporary name `.NAME.tmp`, fsynced and renamed:

1. the authorization record into `authorizations/`;
2. the event *r*;
3. the new current state file (outside the log) replacing the old one;
4. the checkpoint and its metadata, if due;
5. `HEAD.json`;
6. later, out of band, the owner's signed anchor.

**Recovery** classifies a log directory and the current state file (the `recover` command):

* `CONSISTENT`: the event files end at `head_revision`, the log replays (section 11, steps 1-6) and the state file's hash equals
  `head_state_sha256`.
* `ROLL_FORWARD_STATE`: there is exactly one event file beyond `head_revision`, the log up to `head_revision` replays, the next
  event is valid against it (steps 5.1-5.7 succeed when applied to the replayed head), and the state file's hash equals that
  event's `prev_state_sha256`: the state replacement was interrupted; the writer rebuilds the state from the event.
* `ROLL_FORWARD_HEAD`: as above, but the state file's hash equals the event's `new_state_sha256`: only the head (and checkpoint)
  update was interrupted.
* `STOP`: anything else. Recovery never deletes, rewrites or guesses; a STOP goes to the owner.

## 13. Rollback

VLE-1 never removes history. A wrong transition is corrected by a later transition. Returning to the current (full-copy) ledger
procedure needs no conversion, because the current state file keeps the same encoding: the writer stops appending events, and
the log stays as evidence.

## 14. Migration from the current ledger

The genesis of an authoritative VLE-1 log MUST be a byte-for-byte copy of the authoritative state at the cutover revision, and
its first event the first transition after it. Earlier history is **not** converted, and missing earlier snapshots or hash
links MUST NOT be reconstructed or invented. Known gaps are listed in a separate, informative record. A replay of the existing
snapshots through VLE-1 (historical replay) is research evidence only.

## 15. Verifier interface (for conformance testing)

A conforming verifier is a command with three subcommands, each printing exactly one compact JSON line (section 4) to standard
output and exiting 0 whether the input passes or not (a non-zero exit means the verifier itself failed):

* `replay <log-dir> [--from-checkpoint] [--key <public-key-file>]` prints
  `{"status":"PASS","head_revision":N,"head_event_sha256":H,"head_state_sha256":H,"events_replayed":N}` or
  `{"status":"REJECT","code":"E_...","revision":N-or-null}`.
* `recover <log-dir> <state-file>` prints `{"status":"CONSISTENT"}`, `{"status":"ROLL_FORWARD_STATE"}`,
  `{"status":"ROLL_FORWARD_HEAD"}` or `{"status":"STOP"}`.
* `canon <json-file> <eof>` where `<eof>` is `lf` or `none`: prints `{"status":"PASS","sha256":H}`, the hash of
  `S(parse(file), 0) + eof`, or `{"status":"REJECT","code":"E_JSON","revision":null}`.

Error codes: `E_JSON`, `E_ENCODING`, `E_HEAD`, `E_FILES`, `E_GENESIS`, `E_CHECKPOINT`, `E_EVENT`, `E_SEQUENCE`, `E_PREV_EVENT`,
`E_PREV_STATE`, `E_OP`, `E_PATH`, `E_REPLAY`, `E_AUTH`, `E_HEAD_MISMATCH`, `E_ANCHOR_CHAIN`, `E_ANCHOR_SIG`, `E_ANCHOR_HEAD`.

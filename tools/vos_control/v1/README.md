# Development controller foundation v1

This is a bounded library foundation, not migration of existing maintainer
controllers. It has no dependency on historical evidence or private worktrees.
Python standard-library code supports data validation and trusted imports.
Native atomic publication and execution currently require Linux (renameat2 and
waitid/WNOWAIT); unsupported platforms fail closed. Other tests are portable.

## Trust and entry point

bootstrap.py itself must be supplied by a trusted checkout and verified by the
launching controller. Obtain the manifest digest from a separately approved
review/receipt, not from an untrusted config or a freshly self-hashed download:

~~~sh
python3 -I -B tools/vos_control/v1/bootstrap.py \
  --manifest tools/vos_control/v1/module-manifest.json \
  --manifest-pin <independently-approved-SHA256>
~~~

The CLI enters isolated Python mode before importing other standard-library modules.
A non-isolated invocation re-execs through the preloaded frozen OS module or fails
closed; prefer -I from the outset to exclude interpreter-startup hooks too.
This prevents local package-module shadowing. API callers must supply an equally trusted
interpreter environment. The bootstrap verifies every complete module before any package module executes,
then imports from frozen verified bytes under an isolated namespace. It does not
use private controllers, source-text substitutions, arbitrary configured imports
or bytecode caches. The CLI only verifies the package; it never runs a command.
Imports through other paths are outside this trust contract.

Configuration is strict JSON: schema, task_id, baseline (40 hex), allowed_sources,
protected, scratch, evidence and limits. Limits contain timeout_seconds,
max_output_bytes, max_snapshot_bytes, max_files, poll_ms and cleanup_seconds.
Paths are normalized workspace-relative paths. .git is always protected.
Scratch/evidence must be disjoint; sources cannot overlap either or protected paths.

A separate authorization receipt binds the exact raw config hash, package manifest
hash, task, observed baseline/executor, canonical workspace and device/inode
identity, lease, exact argv/executable hashes and resource profile. authorize
requires its independently trusted SHA256 pin. This is pinned local authorization,
not cryptographic signing or proof that JSON originated from an owner. A caller
must never accept an untrusted file as its own authorization pin.

## Requests and supervision

Requests contain exactly: schema=1, task_id, authorization_sha256, positive sequence,
32-hex request_id, operation and payload. check has an empty payload; run names one
explicitly granted command. Filenames bind sequence and ID. A partial file is never
actionable. Publication uses closed synchronized data, same-directory
non-overwriting rename and directory synchronization. A failed post-rename sync
is indeterminate: stop and reconcile the existing file.

Durable ID and sequence claims precede execution. Partial claims, failures and
restarts cannot justify automatically executing the request again. This is
fail-closed replay rejection, not distributed exactly-once delivery: a crash may
burn a request without completing it.

execute requires a caller-supplied, bounded guard returning a validated Observation;
there is no permissive default. The pinned receipt sets task/aggregate ceilings,
minimum reservation margin and physical floors. Observations supply already
computed charges/obligations. No historical ledger formula, retirement policy or
physical reserve is changed.

Execution uses exact argv, a pinned absolute executable, a minimal environment,
bounded output, monotonic timeout and explicit cancellation. Linux WNOWAIT retains
the leader PID until its owned process group is cleaned, avoiding signalling a
reused leader PID. No broad process-name cleanup is performed. Source snapshots
reject unapproved changes and preserve protected paths.

This is detection/supervision of explicitly trusted tools, **not OS confinement**.
It cannot prevent all external writes or malicious daemon/session escapes, and
does not replace native isolation monitors. Guard providers must enforce freshness
and query bounds. Production host adapters, Git-transition brokerage and a
persistent recovery service are deferred. No active workflows migrate automatically.

## Tests and evidence

~~~sh
python3 -B -m unittest discover -s tests/controller -v
~~~

No private directories, volumes, QEMU or kernel builds are required. Optional
VOS_CONTROL_TEST_ROOT supplies an admitted empty fixture root for maintainer runs.
Only owned synthetic fixtures are reset; per-path allocation high-water records
include temporary paths before rename/removal and remain under that root.
Package sources and historical evidence are never cleaned.

Future D1-B3 may consume the accounting input contract. Reservation redesign or
evidence retirement requires separate approval.

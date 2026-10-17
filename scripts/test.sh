#!/usr/bin/env bash
# Smoke-test a production ISO whose kernel halts forever after booting.
# usage: test.sh ISO LOG MARKER...
#
# Boots headless in QEMU and passes only if every MARKER appears on serial,
# no serial line matches FAIL_PATTERN, and QEMU is still running GRACE
# seconds later. QEMU runs with -no-reboot, so a guest reset or crash makes
# it exit, which fails the test. A timeout never passes.
# Env: QEMU (default qemu-system-i386), TIMEOUT (15), GRACE (1),
#      FAIL_PATTERN (extended regex, default 'magic BAD').
set -u
usage="usage: test.sh ISO LOG MARKER..."
ISO=${1:?$usage}
LOG=${2:?$usage}
shift 2
[ $# -gt 0 ] || { echo "$usage" >&2; exit 2; }
EXPECTED=("$@")
QEMU=${QEMU:-qemu-system-i386}
TIMEOUT=${TIMEOUT:-15}
GRACE=${GRACE:-1}
FAIL_PATTERN=${FAIL_PATTERN:-magic BAD}

fail() {
    echo "--- serial output ---"; cat "$LOG" 2>/dev/null; echo "---------------------"
    echo "TEST FAIL: $*" >&2
    exit 1
}

[ -f "$ISO" ] || fail "ISO not found: $ISO"
command -v "$QEMU" >/dev/null || fail "$QEMU not installed"
rm -f "$LOG"

echo "test.sh: $QEMU -cdrom $ISO -display none -serial file:$LOG -no-reboot -boot d"
"$QEMU" -cdrom "$ISO" -display none -serial "file:$LOG" \
    -no-reboot -boot d >/dev/null 2>"$LOG.qemu" &
QPID=$!
trap 'kill "$QPID" 2>/dev/null' EXIT

seen_all() {
    local m
    for m in "${EXPECTED[@]}"; do grep -qF -- "$m" "$LOG" 2>/dev/null || return 1; done
}

alive() { kill -0 "$QPID" 2>/dev/null; }

exited() {
    wait "$QPID"; local rc=$? when="before the expected output"
    seen_all && when="after printing the expected output"
    cat "$LOG.qemu" >&2
    fail "QEMU exited on its own (status $rc) $when; the kernel should halt, not exit or reset"
}

found=0
for ((i = 0; i < TIMEOUT * 10; i++)); do
    alive || exited
    if seen_all; then found=1; break; fi
    sleep 0.1
done
[ "$found" = 1 ] || fail "timed out after ${TIMEOUT}s without expected output"

sleep "$GRACE"
alive || exited
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

grep -qE -- "$FAIL_PATTERN" "$LOG" && fail "serial output matches failure pattern '$FAIL_PATTERN'"

echo "--- serial output ---"; cat "$LOG"; echo "---------------------"
echo "TEST PASS: QEMU booted $ISO and serial output matched"

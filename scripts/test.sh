#!/usr/bin/env bash
# Boot the ISO headless in QEMU and pass only if the kernel's expected serial
# output appears. The kernel halts forever, so QEMU is stopped by this script
# once the output is seen (or after TIMEOUT seconds). A timeout alone never passes.
set -u
ISO=${1:?usage: test.sh ISO LOG}
LOG=${2:?usage: test.sh ISO LOG}
TIMEOUT=${TIMEOUT:-15}
EXPECTED=("Hello from MyOS!" "Multiboot magic OK")

fail() { echo "TEST FAIL: $*" >&2; exit 1; }

[ -f "$ISO" ] || fail "ISO not found: $ISO"
command -v qemu-system-i386 >/dev/null || fail "qemu-system-i386 not installed"
rm -f "$LOG"

qemu-system-i386 -cdrom "$ISO" -display none -serial "file:$LOG" \
    -no-reboot -boot d >/dev/null 2>"$LOG.qemu" &
QPID=$!
trap 'kill "$QPID" 2>/dev/null' EXIT

seen_all() {
    local m
    for m in "${EXPECTED[@]}"; do grep -qF "$m" "$LOG" 2>/dev/null || return 1; done
}

found=0
for ((i = 0; i < TIMEOUT * 10; i++)); do
    if ! kill -0 "$QPID" 2>/dev/null; then
        wait "$QPID"; rc=$?
        seen_all && { found=1; break; }
        cat "$LOG.qemu" >&2
        fail "QEMU exited early (status $rc) before expected output"
    fi
    if seen_all; then found=1; break; fi
    sleep 0.1
done

[ "$found" = 1 ] || { cat "$LOG" 2>/dev/null; fail "timed out after ${TIMEOUT}s without expected output"; }

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
grep -qF "magic BAD" "$LOG" && fail "kernel reported bad Multiboot magic"

echo "--- serial output ---"; cat "$LOG"; echo "---------------------"
echo "TEST PASS: QEMU booted $ISO and serial output matched"

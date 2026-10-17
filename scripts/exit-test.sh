#!/usr/bin/env bash
# Run a self-checking test image (built with MYOS_TEST=1) that reports its
# result through QEMU's isa-debug-exit device: the guest writes V to port
# 0xf4 and QEMU exits with status (V << 1) | 1.
# usage: exit-test.sh NAME ISO EXPECTED_STATUS MARKER [extra QEMU args...]
#
# Passes only if ALL of these hold:
#   - QEMU exits on its own with EXPECTED_STATUS (a timeout never passes),
#   - the serial log contains MARKER,
#   - QEMU logged no triple fault.
# QEMU runs with -no-reboot, so any guest reset makes it exit with status 0,
# which can never equal an isa-debug-exit status (always odd).
# Env: QEMU (default qemu-system-x86_64), MEM (128M), TIMEOUT (20 seconds).
set -u
usage="usage: exit-test.sh NAME ISO EXPECTED_STATUS MARKER [QEMU args...]"
[ $# -ge 4 ] || { echo "$usage" >&2; exit 2; }
NAME=$1 ISO=$2 EXPECT=$3 MARKER=$4
shift 4
QEMU=${QEMU:-qemu-system-x86_64}
MEM=${MEM:-128M}
TIMEOUT=${TIMEOUT:-20}
DIR=$(dirname "$ISO")
LOG=$DIR/$NAME.serial.log
QLOG=$DIR/$NAME.qemu.log

case $EXPECT in
    *[!0-9]* | '') echo "EXPECTED_STATUS must be a number" >&2; exit 2;;
esac
(( EXPECT % 2 == 1 )) || { echo "EXPECTED_STATUS must be odd (isa-debug-exit)" >&2; exit 2; }

fail() {
    echo "--- serial output ---"; cat "$LOG" 2>/dev/null; echo "---------------------"
    local p
    for p in "$@"; do echo "TEST FAIL [$NAME]: $p" >&2; done
    exit 1
}

[ -f "$ISO" ] || fail "ISO not found: $ISO"
command -v "$QEMU" >/dev/null || fail "$QEMU not installed"
rm -f "$LOG" "$QLOG"

cmd=("$QEMU" -cdrom "$ISO" -boot d -m "$MEM" -display none -no-reboot
     -serial "file:$LOG" -device isa-debug-exit,iobase=0xf4,iosize=0x04
     -d cpu_reset,guest_errors -D "$QLOG" "$@")
echo "exit-test.sh [$NAME]: ${cmd[*]}"
timeout --kill-after=5 "$TIMEOUT" "${cmd[@]}" >/dev/null 2>"$DIR/$NAME.qemu.stderr"
status=$?

problems=()
case $status in
    "$EXPECT") ;;
    124 | 137) problems+=("timed out after ${TIMEOUT}s (QEMU killed)");;
    *) problems+=("QEMU exit status $status, expected $EXPECT");;
esac
grep -qF -- "$MARKER" "$LOG" 2>/dev/null || problems+=("serial marker not found: '$MARKER'")
grep -qi 'triple fault' "$QLOG" 2>/dev/null && problems+=("QEMU logged a triple fault")

[ ${#problems[@]} -eq 0 ] || fail "${problems[@]}"
echo "--- serial output ---"; cat "$LOG"; echo "---------------------"
echo "TEST PASS [$NAME]: exit status $status and marker '$MARKER' found"

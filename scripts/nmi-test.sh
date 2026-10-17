#!/usr/bin/env bash
# Inject one genuine NMI into a test image through the QEMU monitor (HMP
# command "nmi") once the guest has printed WAIT_MARKER, then judge the run
# with exit-test.sh. QEMU delivers it as a real NMI (not "int $2"), so this
# exercises the asynchronous path to the NMI gate and its IST stack.
# usage: nmi-test.sh NAME ISO EXPECTED_STATUS WAIT_MARKER MARKER... -- [QEMU args...]
set -u
[ $# -ge 6 ] || { echo "usage: nmi-test.sh NAME ISO STATUS WAIT_MARKER MARKER... -- [QEMU args]" >&2; exit 2; }
NAME=$1 ISO=$2 STATUS=$3 WAIT=$4
shift 4
DIR=$(dirname "$ISO")
LOG=$DIR/$NAME.serial.log
SOCK=$DIR/$NAME.monitor
rm -f "$LOG" "$SOCK"

(
    for _ in $(seq 1 300); do               # at most ~30 s
        grep -aqF -- "$WAIT" "$LOG" 2>/dev/null && break
        sleep 0.1
    done
    if ! grep -aqF -- "$WAIT" "$LOG" 2>/dev/null; then
        echo "nmi-test: '$WAIT' never appeared; no NMI sent" >&2
        exit 1
    fi
    sleep 0.5                               # let the guest reach hlt
    python3 -I - "$SOCK" <<'PY' && echo "nmi-test: sent 'nmi' to the QEMU monitor"
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(b"nmi\n")
time.sleep(0.5)
s.close()
PY
) &
injector=$!
scripts/exit-test.sh "$NAME" "$ISO" "$STATUS" "$@" -monitor "unix:$SOCK,server=on,wait=off"
status=$?
wait "$injector" || status=1
rm -f "$SOCK"
exit $status

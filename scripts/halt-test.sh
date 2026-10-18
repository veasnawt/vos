#!/usr/bin/env bash
# Run an image whose kernel ends in a halt (the production kernel, or a panic
# with no exit device) and judge its final state; a timeout alone never passes.
# usage: halt-test.sh NAME ISO KERNEL SECONDS END MARKER... -- [extra QEMU args]
#   KERNEL   the vos.bin inside ISO (its symbols locate the halt)
#   END      main:  halted right after kernel_main returned (a complete boot)
#            panic: halted inside panic()
#   MARKER   must appear in a non-"user[" serial line ("user[..." markers in
#            user lines); with SERIAL=0 there is no serial port and no marker
# After SECONDS the QEMU monitor records the CPU state (and a screenshot).
# Env: OVMF=1 (q35 + OVMF, fresh variable store per run; see exit-test.sh),
#      MEM (default 1G), SPARSE_MEM=1, MEDIA=usb (the ISO as a USB stick;
#      default CD-ROM), SERIAL=0, SHOT=1 (screenshot $DIR/$NAME.png),
#      COLOR=RRGGBB[,RRGGBB...] (each colour must cover >= 64 pixels of the
#      screenshot; implies SHOT=1), QEMU.
set -u
[ $# -ge 5 ] || { echo "usage: halt-test.sh NAME ISO KERNEL SECONDS END MARKER... -- [QEMU args]" >&2; exit 2; }
NAME=$1 ISO=$2 KERNEL=$3 SECS=$4 END=$5
shift 5
MARKERS=() QARGS=()
while [ $# -gt 0 ] && [ "$1" != -- ]; do MARKERS+=("$1"); shift; done
[ $# -gt 0 ] && shift
QARGS=("$@")
QEMU=${QEMU:-qemu-system-x86_64}
MEM=${MEM:-1G}
[ -f "$ISO" ] && ISO=$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")     # QEMU runs inside DIR (below)
[ -f "$KERNEL" ] && KERNEL=$(cd "$(dirname "$KERNEL")" && pwd)/$(basename "$KERNEL")
DIR=$(dirname "$ISO")
LOG=$DIR/$NAME.serial.log MON=$DIR/$NAME.monitor.log PPM=$DIR/$NAME.ppm PNG=$DIR/$NAME.png
COLOR=${COLOR:-}
[ -n "$COLOR" ] && SHOT=1
SHOT=${SHOT:-0}

fail() {
    echo "--- serial output ---"; cat "$LOG" 2>/dev/null; echo "---------------------"
    for p in "$@"; do echo "TEST FAIL [$NAME]: $p" >&2; done
    exit 1
}
[ -f "$ISO" ] || fail "ISO not found: $ISO"
[ -f "$KERNEL" ] || fail "kernel not found: $KERNEL"
rm -f "$LOG" "$MON" "$PPM" "$PNG"

FW=()
if [ "${OVMF:-0}" = 1 ]; then
    CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
    VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
    [ -f "$CODE" ] && [ -f "$VARS" ] || fail "OVMF firmware not found ($CODE, $VARS)"
    cp "$VARS" "$DIR/$NAME.vars.fd" || fail "cannot copy the OVMF variable store"
    FW=(-machine q35 -drive "if=pflash,format=raw,readonly=on,file=$CODE"
        -drive "if=pflash,format=raw,file=$DIR/$NAME.vars.fd")
fi
MEMARGS=(-m "$MEM")
if [ "${SPARSE_MEM:-0}" = 1 ]; then
    rm -f "$DIR/$NAME.ram"
    MEMARGS=(-m "$MEM" -object "memory-backend-file,id=ram,size=$MEM,mem-path=$DIR/$NAME.ram,share=on"
             -machine memory-backend=ram)
fi
case ${MEDIA:-cdrom} in
    usb) DISK=(-drive "if=none,id=stick,format=raw,readonly=on,file=$ISO" -device qemu-xhci
               -device usb-storage,drive=stick,bootindex=0) ;;
    *)   DISK=(-cdrom "$ISO" -boot d) ;;
esac
SER=(-serial "file:$LOG")
[ "${SERIAL:-1}" = 0 ] && SER=(-serial none)

# The monitor writes relative file names (it rejects some absolute paths): run inside DIR.
shot_cmd=""
[ "$SHOT" = 1 ] && shot_cmd="screendump $NAME.ppm"
echo "halt-test.sh [$NAME]: $QEMU ${FW[*]} ${MEMARGS[*]} ${DISK[*]} ${SER[*]} ${QARGS[*]}"
(cd "$DIR" && { sleep "$SECS"; [ -n "$shot_cmd" ] && echo "$shot_cmd"; echo "info registers"; sleep 2; echo quit; } |
    timeout --kill-after=5 $((SECS + 30)) "$QEMU" "${FW[@]}" "${MEMARGS[@]}" "${DISK[@]}" -display none -no-reboot \
        "${SER[@]}" -monitor stdio "${QARGS[@]}" > "$NAME.monitor.log" 2> "$NAME.qemu.stderr")
status=$?
rm -f "$DIR/$NAME.ram"
[ $status = 0 ] || fail "QEMU did not stay up until the check (exit status $status)"

problems=()
rip=$(sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$MON" | grep -ao 'RIP=[0-9a-f]*' | head -1 | cut -d= -f2)
hlt=$(sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$MON" | grep -ao 'HLT=[01]' | head -1 | cut -d= -f2)
[ -n "$rip" ] || problems+=("no CPU state recorded")
[ "$hlt" = 1 ] || problems+=("CPU not halted at the check (RIP ${rip:-?}): running or hung")
case $END in
    main)
        # kernel_main returns to "cli; hlt; jmp" in boot.S: a halted CPU reports the jmp after the hlt.
        start=$(nm "$KERNEL" | awk '/ higher_half_entry$/{print $1}')
        want=$(objdump -d --start-address=0x$start --stop-address=$(printf '0x%x' $((0x$start + 0x80))) "$KERNEL" |
               awk '/call.*<kernel_main>/{getline; getline; getline; split($1, a, ":"); print a[1]; exit}')
        [ -n "$want" ] && [ -n "$rip" ] && [ "$((16#$rip))" = "$((16#$want))" ] ||
            problems+=("halted at ${rip:-?}, not after kernel_main's return (${want:-?})") ;;
    panic)
        read -r pstart psize < <(nm -S "$KERNEL" | awk '/ T panic$/{print $1, $2}')
        [ -n "${pstart:-}" ] && [ -n "$rip" ] && [ "$((16#$rip))" -ge "$((16#$pstart))" ] &&
            [ "$((16#$rip))" -lt "$((16#$pstart + 16#$psize))" ] || problems+=("halted at ${rip:-?}, not inside panic()") ;;
    *) problems+=("END must be main or panic") ;;
esac
for m in "${MARKERS[@]}"; do
    case $m in
        'user['*) grep -a '^user\[' "$LOG" 2>/dev/null | grep -qaF -- "$m" ;;
        *)        grep -av '^user\[' "$LOG" 2>/dev/null | grep -qaF -- "$m" ;;
    esac || problems+=("serial marker not found: '$m'")
done
if [ "$SHOT" = 1 ]; then
    if [ -f "$PPM" ]; then
        out=$(python3 -I - "$PPM" "$PNG" "$COLOR" <<'PY'
import struct, sys, zlib
ppm, png, colors = sys.argv[1], sys.argv[2], [c for c in sys.argv[3].split(",") if c]
d = open(ppm, "rb").read()
magic, dims, maxv, px = d.split(b"\n", 3)
w, h = map(int, dims.split())
rows = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
chunk = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
open(png, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                      chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
pix = [px[i:i + 3] for i in range(0, w * h * 3, 3)]
missing = [c for c in colors if sum(1 for p in pix if p == bytes.fromhex(c)) < 64]
print("%dx%d" % (w, h), "missing:" + ",".join(missing) if missing else "colours ok")
PY
)
        echo "screenshot $PNG: $out"
        case $out in *missing:*) problems+=("screenshot lacks colour(s): ${out#*missing:}") ;; esac
    else
        problems+=("no screenshot taken")
    fi
fi

[ ${#problems[@]} -eq 0 ] || fail "${problems[@]}"
echo "TEST PASS [$NAME]: halted as expected ($END, RIP $rip), ${#MARKERS[@]} marker(s) found"

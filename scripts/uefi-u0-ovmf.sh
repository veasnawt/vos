#!/usr/bin/env bash
# U0 (experimental): boot one image under QEMU + OVMF (Secure Boot not enabled) and
# judge the end state. See docs/plans/uefi.md.
# usage: uefi-u0-ovmf.sh NAME ISO SECONDS cdrom|usb [extra QEMU args]
#   OVMF_CODE (default /usr/share/OVMF/OVMF_CODE_4M.fd, used read-only)
#   OVMF_VARS (default /usr/share/OVMF/OVMF_VARS_4M.fd, copied fresh for every run)
#   OUT       (default build/x86_64-uefi-u0)
# After SECONDS the QEMU monitor records "info cpus" and "info registers". Verdict:
#   halted at the instruction after kernel_main's return -> "kernel returned and halted"
#   halted elsewhere                                   -> "halted elsewhere (panic?)"
#   not halted                                         -> "still running or hung"
set -u
cd "$(dirname "$0")/.."
n=$1 iso=$2 t=$3 media=$4; shift 4
OUT=${OUT:-build/x86_64-uefi-u0}
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
cp "${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" "$OUT/$n.vars.fd"
case $media in
cdrom) m=(-drive if=none,id=cd,format=raw,readonly=on,media=cdrom,file=$iso -device ide-cd,drive=cd,bootindex=0) ;;
usb)   m=(-drive if=none,id=stick,format=raw,readonly=on,file=$iso -device qemu-xhci -device usb-storage,drive=stick,bootindex=0) ;;
*)     echo "media: cdrom or usb" >&2; exit 2 ;;
esac
(sleep "$t"; echo "info cpus"; echo "info registers"; sleep 1; echo quit) |
    timeout --kill-after=5 $((t + 20)) qemu-system-x86_64 -machine q35 -cpu Haswell-noTSX -m 1G -nic none \
        -display none -no-reboot -monitor stdio \
        -drive if=pflash,format=raw,readonly=on,file="$CODE" -drive if=pflash,format=raw,file="$OUT/$n.vars.fd" \
        "${m[@]}" -serial file:"$OUT/$n.serial.log" -d cpu_reset,guest_errors -D "$OUT/$n.qemu.log" "$@" \
        > "$OUT/$n.monitor.log" 2> "$OUT/$n.stderr"
ret=$(nm build/x86_64/vos.bin | awk '/ higher_half_entry$/{print $1}')
# kernel_main returns to "cli; hlt; jmp" right after the call; a halted CPU reports the jmp after the hlt.
want=$(objdump -d --start-address=0x$ret --stop-address=$(printf '0x%x' $((0x$ret + 0x80))) build/x86_64/vos.bin |
       awk '/call.*<kernel_main>/{getline; getline; getline; split($1, a, ":"); print a[1]; exit}')
rip=$(grep -ao 'RIP=[0-9a-f]*' "$OUT/$n.monitor.log" | head -1 | cut -d= -f2)
hlt=$(grep -ao 'HLT=[01]' "$OUT/$n.monitor.log" | head -1 | cut -d= -f2)
if [ "$hlt" = 1 ] && [ "$((16#${rip:-0}))" = "$((16#${want:-1}))" ]; then v="kernel returned and halted"
elif [ "$hlt" = 1 ]; then v="halted elsewhere (panic?) at $rip"
else v="still running or hung at ${rip:-?}"; fi
echo "$n: $v"
grep -a "panic:\|boot: checks passed\|boot: FAILED\|VOS-TEST" "$OUT/$n.serial.log" | tr -d '\r'

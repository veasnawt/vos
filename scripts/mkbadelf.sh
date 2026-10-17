#!/usr/bin/env bash
# Make a malformed copy of a valid VOS executable for the N1.9 isolation suite.
# usage: mkbadelf.sh IN OUT KIND
# KIND: magic | dyn | wx | entry | abi | interp | trunc | overlap
# Fields are located through the program headers of IN (3 PT_LOAD, PT_NOTE,
# PT_GNU_STACK in that order, as user/lib/user.ld produces).
set -eu
[ $# -eq 3 ] || { echo "usage: mkbadelf.sh IN OUT KIND" >&2; exit 2; }
IN=$1 OUT=$2 KIND=$3
get() { local h="" b; for b in $(od -An -v -t x1 -j "$1" -N "$2" -- "$IN"); do h="$b$h"; done; echo $((16#$h)); }
put() {
    local off=$1 len=$2 v=$3 i s=""
    for (( i = 0; i < len; i++ )); do s+=$(printf '\\x%02x' $(( (v >> (8 * i)) & 0xff ))); done
    printf "$s" | dd of="$OUT" bs=1 seek="$off" conv=notrunc status=none
}
PHOFF=$(get 32 8)
TEXT=$PHOFF RO=$((PHOFF + 56)) NOTE=$((PHOFF + 168)) STACK=$((PHOFF + 224))
(( $(get $TEXT 4) == 1 && $(get $RO 4) == 1 && $(get $NOTE 4) == 4 )) || { echo "mkbadelf: unexpected layout in $IN" >&2; exit 1; }
cp "$IN" "$OUT"
case $KIND in
    magic)   put 0 1 0x7E ;;
    dyn)     put 16 2 3 ;;
    wx)      put $((TEXT + 4)) 4 7 ;;
    entry)   put 24 8 $((0x401000)) ;;
    abi)     put $(( $(get $((NOTE + 8)) 8) + 16 )) 4 1 ;;
    interp)  put "$STACK" 4 3 ;;
    trunc)   truncate -s 200 "$OUT" ;;
    overlap) put $((RO + 16)) 8 $((0x400000)) ;;
    *) echo "mkbadelf: unknown kind $KIND" >&2; rm -f "$OUT"; exit 2 ;;
esac

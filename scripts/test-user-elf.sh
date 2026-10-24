#!/usr/bin/env bash
# Build and negative tests for VOS user executables (N1.7; static only, nothing runs).
# usage: test-user-elf.sh USER_BUILD_DIR KERNEL_ELF...
# Env (from user/user.mk): USER_CC, USER_CFLAGS, USER_LD, USER_LDFLAGS (without -T),
#   USER_LDS, KERNEL_SYSCALL_H, USER_SYSCALL_H, ELFCHECK (host build of the kernel's
#   arch/x86_64/elf.c).
#
# 1. hello.elf is accepted, has the expected profile, uses int $0x80 only in
#    the runtime stubs, matches the kernel's ABI numbers, and is not part of
#    any kernel image.
# 2. Every single-field corruption of hello.elf is rejected for its own reason.
# 3. Executables built by the real toolchain with a forbidden property are
#    rejected (or fail to link, where the linker script forbids them).
# 4. The kernel's validator (ELFCHECK) reaches the same verdict as this
#    script's loader profile on every file above, except the files rejected
#    only by build-time checks it cannot make (relocations, x87, TLS sections).
set -u
[ $# -ge 2 ] || { echo "usage: test-user-elf.sh USER_BUILD_DIR KERNEL_ELF..." >&2; exit 2; }
UB=$1; shift
KERNELS=("$@")
CHECK=scripts/check-user-elf.sh
HELLO=$UB/hello.elf
T=$UB/neg
rm -rf "$T"; mkdir -p "$T"
pass=0 fail=0

ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }

# expect_reject FILE REASON: the validator must reject FILE with REASON in its message.
expect_reject() {
    local out rc
    out=$("$CHECK" "$1" 2>&1); rc=$?
    if (( rc == 1 )) && grep -qF -- "$2" <<<"$out"; then ok
    else bad "$(basename "$1"): expected rejection '$2', got rc=$rc: $out"; fi
}

# put FILE OFF LEN VALUE: write VALUE little-endian.
put() {
    local f=$1 off=$2 len=$3 v=$4 i s=""
    for (( i = 0; i < len; i++ )); do s+=$(printf '\\x%02x' $(( (v >> (8 * i)) & 0xff ))); done
    printf "$s" | dd of="$f" bs=1 seek="$off" conv=notrunc status=none
}
get() { # FILE OFF LEN -> decimal (small values only)
    local h="" b
    for b in $(od -An -v -t x1 -j "$2" -N "$3" -- "$1"); do h="$b$h"; done
    echo $((16#$h))
}

PHOFF=$(get "$HELLO" 32 8)
PHNUM=$(get "$HELLO" 56 2)
SIZE=$(stat -L -c %s "$HELLO")
# Index of the n-th (0-based) program header of a type in hello.elf.
ph() {
    local want=$1 nth=${2:-0} i
    for (( i = 0; i < PHNUM; i++ )); do
        if (( $(get "$HELLO" $((PHOFF + i * 56)) 4) == want )); then
            (( nth-- == 0 )) && { echo $((PHOFF + i * 56)); return; }
        fi
    done
    echo -1
}
TEXT=$(ph 1 0) RO=$(ph 1 1) DATA=$(ph 1 2) NOTE=$(ph 4) STACK=$(ph $((0x6474e551)))
(( TEXT > 0 && RO > 0 && DATA > 0 && NOTE > 0 && STACK > 0 )) || { echo "FAIL: unexpected hello.elf layout" >&2; exit 1; }
NOTE_OFF=$(get "$HELLO" $((NOTE + 8)) 8)
TEXT_FILESZ=$(get "$HELLO" $((TEXT + 32)) 8)

# mutate NAME REASON FIELD-WRITES...: copy hello.elf, apply "OFF LEN VALUE" triples, expect rejection.
mutate() {
    local name=$1 reason=$2 f=$T/$1.elf
    shift 2
    cp "$HELLO" "$f"
    while [ $# -ge 3 ]; do put "$f" "$1" "$2" "$3"; shift 3; done
    expect_reject "$f" "$reason"
}

# ---- 1. the real executable ----
"$CHECK" "$HELLO" >/dev/null 2>&1 && ok || bad "hello.elf rejected: $("$CHECK" "$HELLO" 2>&1)"
hdr=$(LC_ALL=C readelf -hW "$HELLO")
grep -q 'Class: *ELF64' <<<"$hdr" && grep -q 'Machine: *Advanced Micro Devices X86-64' <<<"$hdr" &&
    grep -q 'Type: *EXEC' <<<"$hdr" && ok || bad "readelf: hello.elf is not ELF64 x86-64 EXEC"
notes=$(LC_ALL=C readelf -nW "$HELLO")
grep -q '^ *VOS *0x00000004' <<<"$notes" && grep -q 'description data: 00 00 00 00' <<<"$notes" &&
    ok || bad "readelf: VOS ABI note not found"
(( $(LC_ALL=C nm -u "$HELLO" | wc -l) == 0 )) && ok || bad "hello.elf has undefined symbols"
[[ $(LC_ALL=C nm -u "$UB/hello/hello.o" | awk '{print $2}' | sort | tr '\n' ' ') == "vos_debug_write " ]] &&
    ok || bad "hello.o needs symbols other than vos_debug_write"
# int $0x80 only inside the runtime stubs.
outside=$(LC_ALL=C objdump -d --no-show-raw-insn "$HELLO" |
    awk '/^[0-9a-f]+ <.*>:$/ { fn = $2 } /\tint +\$0x80/ && fn != "<vos_debug_write>:" && fn != "<vos_exit>:" { n++ } END { print n + 0 }')
(( outside == 0 )) && ok || bad "int \$0x80 outside vos_exit/vos_debug_write ($outside)"
# ABI numbers: the user header must agree with the kernel's.
for pair in "SYS_EXIT VOS_SYS_EXIT" "SYS_DEBUG_WRITE VOS_SYS_DEBUG_WRITE" "VOS_ENOSYS VOS_ENOSYS" \
            "VOS_EINVAL VOS_EINVAL" "VOS_EFAULT VOS_EFAULT" "DEBUG_WRITE_MAX VOS_DEBUG_WRITE_MAX"; do
    set -- $pair
    k=$(awk -v n="$1" '$1 == "#define" && $2 == n { print $3 }' "$KERNEL_SYSCALL_H")
    u=$(awk -v n="$2" '$1 == "#define" && $2 == n { print $3 }' "$USER_SYSCALL_H")
    [[ -n $k && $k == "$u" ]] && ok || bad "ABI constant $1=$k vs $2=$u"
done
# Not part of any kernel: no runtime symbols, and hello's code bytes absent.
LC_ALL=C objcopy -O binary --only-section=.text "$HELLO" "$T/hello.text"
code=$(od -An -v -t x1 "$T/hello.text" | tr -d ' \n')
(( ${#code} > 32 )) || bad "could not extract hello's code"
for k in "${KERNELS[@]}"; do
    [ -f "$k" ] || { bad "kernel image missing: $k"; continue; }
    LC_ALL=C nm "$k" 2>/dev/null | grep -qE ' (main|vos_exit|vos_debug_write|__vos_entry_rsp)$' &&
        bad "$k defines user runtime symbols" || ok
    od -An -v -t x1 "$k" | tr -d ' \n' | grep -qF -- "$code" && bad "$k contains hello's code" || ok
done

# ---- 2. single-field corruptions of hello.elf ----
mutate magic          "bad ELF magic"            0 1 0x7e
mutate class32        "not ELFCLASS64"           4 1 1
mutate bigendian      "not little-endian"        5 1 2
mutate ei-version     "EI_VERSION"               6 1 2
mutate osabi          "EI_OSABI"                 7 1 3
mutate abiversion     "EI_ABIVERSION"            8 1 1
mutate pad            "EI_PAD"                   9 1 1
mutate et-dyn         "ET_DYN"                   16 2 3
mutate et-rel         "expected ET_EXEC"         16 2 1
mutate machine        "e_machine"                18 2 3
mutate e-version      "e_version"                20 4 2
mutate e-flags        "e_flags"                  48 4 1
mutate ehsize         "e_ehsize"                 52 2 52
mutate phentsize      "e_phentsize"              54 2 32
mutate phnum0         "e_phnum"                  56 2 0
mutate phnum9         "e_phnum"                  56 2 9
mutate phoff-header   "overlap the ELF header"   32 8 8
mutate phoff-outside  "outside the file"         32 8 "$SIZE"
mutate phoff-huge     "out of range"             32 8 $((1 << 62))
mutate entry-zero     "entry"                    24 8 0
mutate entry-rodata   "entry"                    24 8 $((0x401000))
mutate entry-past     "entry"                    24 8 $((0x400000 + TEXT_FILESZ))
mutate text-wx        "writable and executable"  $((TEXT + 4)) 4 7
mutate text-noread    "not readable"             $((TEXT + 4)) 4 1
mutate text-flags     "unknown flags"            $((TEXT + 4)) 4 $((0x15))
mutate no-exec        "no executable PT_LOAD"    $((TEXT + 4)) 4 4
mutate load-align     "p_align"                  $((TEXT + 48)) 8 $((0x200000))
mutate vaddr-unal     "not page-aligned"         $((TEXT + 16)) 8 $((0x400010))
mutate offset-unal    "not page-aligned"         $((TEXT + 8)) 8 $((0x1004))
mutate filesz-gt-mem  "p_filesz > p_memsz"       $((DATA + 32)) 8 16
mutate memsz-zero     "empty segment"            $((RO + 32)) 8 0  $((RO + 40)) 8 0
mutate vaddr-low      "outside [0x400000"        $((TEXT + 16)) 8 $((0x1000))
mutate vaddr-high     "outside [0x400000"        $((DATA + 16)) 8 $((0x3FFFF000))  $((DATA + 40)) 8 $((0x2000))
mutate overlap        "unsorted or overlapping"  $((RO + 16)) 8 $((0x400000))
mutate unsorted       "unsorted or overlapping"  $((TEXT + 16)) 8 $((0x403000))
mutate file-range     "file range outside"       $((DATA + 32)) 8 $((0x10000))  $((DATA + 40)) 8 $((0x10000))
mutate memsz-huge     "out of range"             $((DATA + 40)) 8 $((1 << 63))
mutate image-1mib     "more than 1 MiB"          $((DATA + 40)) 8 $((0x110000))
mutate stack-exec     "executable stack"         $((STACK + 4)) 4 7
mutate interp         "PT_INTERP"                $STACK 4 3
mutate dynamic        "PT_DYNAMIC"               $STACK 4 2
mutate tls            "PT_TLS"                   $STACK 4 7
mutate phdr           "PT_PHDR"                  $STACK 4 6
mutate shlib          "PT_SHLIB"                 $STACK 4 5
mutate relro          "PT_GNU_RELRO"             $STACK 4 $((0x6474e552))
mutate property       "PT_GNU_PROPERTY"          $STACK 4 $((0x6474e553))
mutate eh-frame       "PT_GNU_EH_FRAME"          $STACK 4 $((0x6474e550))
mutate null-phdr      "unknown program-header type 0x0" $STACK 4 0
mutate unknown-phdr   "unknown program-header type"     $STACK 4 $((0x60000000))
mutate five-loads     "more than 4 PT_LOAD" \
    "$NOTE" 4 1  $((NOTE + 4)) 4 4  $((NOTE + 8)) 8 $((0x2000))  $((NOTE + 16)) 8 $((0x404000)) \
    $((NOTE + 32)) 8 8  $((NOTE + 40)) 8 8  $((NOTE + 48)) 8 $((0x1000)) \
    "$STACK" 4 1  $((STACK + 4)) 4 4  $((STACK + 8)) 8 $((0x2000))  $((STACK + 16)) 8 $((0x405000)) \
    $((STACK + 32)) 8 8  $((STACK + 40)) 8 8  $((STACK + 48)) 8 $((0x1000))
mutate note-align     "PT_NOTE"                  $((NOTE + 48)) 8 8
mutate note-outside   "PT_NOTE"                  $((NOTE + 8)) 8 "$SIZE"
mutate note-trailing  "trailing bytes"           $((NOTE + 32)) 8 24
mutate note-name      "no VOS ABI note"          $((NOTE_OFF + 12)) 1 $((0x58))
mutate note-type      "no VOS ABI note"          $((NOTE_OFF + 8)) 4 2
# Preserve the former note-version.elf bytes/path; 2 is now supported.
cp "$HELLO" "$T/note-version.elf"; put "$T/note-version.elf" $((NOTE_OFF + 16)) 4 2
"$CHECK" "$T/note-version.elf" >/dev/null && ok || bad "ABI 2 rejected"
mutate note-future    "version 4"                $((NOTE_OFF + 16)) 4 4
mutate note-descsz    "note larger than the segment"  $((NOTE_OFF + 4)) 4 8
mutate note-namesz    "note larger than the segment"  "$NOTE_OFF" 4 $((0xFFFFFFFF))
cp "$HELLO" "$T/truncated.elf"; truncate -s 200 "$T/truncated.elf"
expect_reject "$T/truncated.elf" "program-header table outside the file"
cp "$HELLO" "$T/tiny.elf"; truncate -s 40 "$T/tiny.elf"
expect_reject "$T/tiny.elf" "smaller than an ELF header"
cp "$HELLO" "$T/large.elf"; truncate -s $((0x100001)) "$T/large.elf"
expect_reject "$T/large.elf" "larger than 1 MiB"

# ---- 3. executables built by the real toolchain ----
RT="$UB/lib/crt0.o $UB/lib/syscall.o"
NOTE_O=$UB/lib/abi_note.o
HELLO_O=$UB/hello/hello.o
link() { # OUT SCRIPT OBJS...
    local out=$1 lds=$2; shift 2
    $USER_LD $USER_LDFLAGS -T "$lds" -o "$out" "$@" 2>"$out.ldlog"
}
# The real linker script refuses a missing or duplicated note; linked without
# its assertions, the validator must reject the result.
grep -v 'ASSERT(' "$USER_LDS" > "$T/noassert.ld"
link_refused() { # NAME MESSAGE OBJS...
    local name=$1 msg=$2; shift 2
    if link "$T/$name.elf" "$USER_LDS" "$@"; then bad "$name: the linker script accepted it"
    else grep -qF -- "$msg" "$T/$name.elf.ldlog" && ok || bad "$name: wrong link error"; fi
}
link_refused tc-no-note "exactly one VOS ABI note" $RT "$HELLO_O"
link_refused tc-two-notes "exactly one VOS ABI note" $RT "$NOTE_O" "$NOTE_O" "$HELLO_O"
link "$T/tc-no-note-raw.elf" "$T/noassert.ld" $RT "$HELLO_O" &&
    expect_reject "$T/tc-no-note-raw.elf" "PT_NOTE" || bad "tc-no-note-raw: link failed"
link "$T/tc-two-notes-raw.elf" "$T/noassert.ld" $RT "$NOTE_O" "$NOTE_O" "$HELLO_O" &&
    expect_reject "$T/tc-two-notes-raw.elf" "more than one VOS ABI note" || bad "tc-two-notes-raw: link failed"
sed 's/text    PT_LOAD FLAGS(5)/text    PT_LOAD FLAGS(7)/' "$USER_LDS" > "$T/wx.ld"
link "$T/tc-wx.elf" "$T/wx.ld" $RT "$NOTE_O" "$HELLO_O" &&
    expect_reject "$T/tc-wx.elf" "writable and executable" || bad "tc-wx: link failed"
sed 's/stack   PT_GNU_STACK FLAGS(6)/stack   PT_GNU_STACK FLAGS(7)/' "$USER_LDS" > "$T/xstack.ld"
link "$T/tc-xstack.elf" "$T/xstack.ld" $RT "$NOTE_O" "$HELLO_O" &&
    expect_reject "$T/tc-xstack.elf" "executable stack" || bad "tc-xstack: link failed"
$USER_LD $USER_LDFLAGS -q -T "$USER_LDS" -o "$T/tc-relocs.elf" $RT "$NOTE_O" "$HELLO_O" &&
    expect_reject "$T/tc-relocs.elf" "relocation sections present" || bad "tc-relocs: link failed"
# A real PIE: position-independent code linked by ld's default script.
$USER_CC ${USER_CFLAGS/-fno-pie/-fpie} -fpie -c user/hello/hello.c -o "$T/hello-pie.o" &&
    $USER_LD -m elf_x86_64 -nostdlib -pie --no-dynamic-linker -z noexecstack -o "$T/tc-pie.elf" \
        $RT "$NOTE_O" "$T/hello-pie.o" &&
    expect_reject "$T/tc-pie.elf" "ET_DYN" || bad "tc-pie: build failed"
# x87 code in main.
$USER_CC $USER_CFLAGS -x assembler-with-cpp -c user/tests/x87_main.S -o "$T/x87_main.o" &&
    link "$T/tc-x87.elf" "$USER_LDS" $RT "$NOTE_O" "$T/x87_main.o" &&
    expect_reject "$T/tc-x87.elf" "x87/MMX/SSE" || bad "tc-x87: build failed"
# SSE code in main (an xmm register operand, no x87 mnemonic).
$USER_CC $USER_CFLAGS -x assembler-with-cpp -c user/tests/sse_main.S -o "$T/sse_main.o" &&
    link "$T/tc-sse.elf" "$USER_LDS" $RT "$NOTE_O" "$T/sse_main.o" &&
    expect_reject "$T/tc-sse.elf" "x87/MMX/SSE" || bad "tc-sse: build failed"
# Thread-local storage: the real linker script refuses it; without the assertion the validator does.
$USER_CC $USER_CFLAGS -c user/tests/tls_main.c -o "$T/tls_main.o" || bad "tls_main.o: compile failed"
link_refused tc-tls "VOS ABI v0 has no thread-local storage" $RT "$NOTE_O" "$T/tls_main.o"
link "$T/tc-tls-noassert.elf" "$T/noassert.ld" $RT "$NOTE_O" "$T/tls_main.o" &&
    expect_reject "$T/tc-tls-noassert.elf" "thread-local storage" || bad "tc-tls-noassert: link failed"

# ---- 4. kernel validator (host build) versus this validator ----
build_time_only=" tc-relocs.elf tc-x87.elf tc-sse.elf tc-tls-noassert.elf "
same=0 compared=0
for f in "$HELLO" "$T"/*.elf; do
    "$CHECK" "$f" >/dev/null 2>&1; shell=$?
    "$ELFCHECK" "$f" >/dev/null 2>&1; kernel=$?
    (( shell <= 1 && kernel <= 1 )) || { bad "$(basename "$f"): validator error ($shell/$kernel)"; continue; }
    compared=$((compared + 1))
    if [[ $build_time_only == *" $(basename "$f") "* ]]; then
        (( shell == 1 && kernel == 0 )) && ok ||
            bad "$(basename "$f"): expected build-time-only rejection (shell 1, kernel 0), got $shell/$kernel"
    elif (( shell == kernel )); then
        same=$((same + 1)); ok
    else
        bad "$(basename "$f"): shell validator $shell, kernel validator $kernel"
    fi
done
echo "test-user-elf: kernel validator agrees on $same of $compared files (4 build-time-only differences expected)"

# ---- 5. isolation-suite fixtures (N1.9): the expected static verdicts ----
# Valid fixtures pass both validators; x87/sse pass the load-time profile but
# not the build-time instruction check (the suite shows the CPU still traps
# them); bad-* fail both, each for its own rule.
if [ -n "${SUITE_DIR:-}" ]; then
    for f in "$SUITE_DIR"/*.elf; do
        n=$(basename "$f" .elf)
        out=$("$CHECK" "$f" 2>&1); shell=$?
        "$ELFCHECK" "$f" >/dev/null 2>&1; kernel=$?
        case $n in
            x87|sse) (( shell == 1 && kernel == 0 )) && grep -q 'x87/MMX/SSE' <<<"$out" && ok ||
                         bad "suite/$n: expected build-time-only rejection, got $shell/$kernel: $out" ;;
            bad-abi) (( shell == 0 && kernel == 0 )) && ok || bad "frozen ABI 2 fixture should now be accepted" ;;
            bad-*)   (( shell == 1 && kernel == 1 )) && ok || bad "suite/$n: expected rejection by both, got $shell/$kernel" ;;
            *)       (( shell == 0 && kernel == 0 )) && ok || bad "suite/$n: expected acceptance, got $shell/$kernel: $out" ;;
        esac
    done
fi

echo "test-user-elf: $pass passed, $fail failed"
(( fail == 0 ))

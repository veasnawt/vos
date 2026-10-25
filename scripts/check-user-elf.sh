#!/usr/bin/env bash
# Validate a VOS ABI v0 or v1 user executable (static ELF64 x86_64, plan section 8.4;
# ABI v1 since N2.4 adds system calls only, so the profile is the same).
# usage: check-user-elf.sh FILE
# Exit 0: accepted. Exit 1: rejected (first reason on stderr). Exit 2: usage.
#
# Part 1 is the loader profile: exactly what the N1.8 loader will enforce. It
# reads the ELF header, program headers and notes as bytes (od), never section
# headers and never text printed by other tools. Every 64-bit field must be
# below 2^40 (far above any N1 limit), so shell arithmetic cannot overflow.
#
# Part 2 adds build-time checks the loader cannot make because they need
# section headers or disassembly: no relocation or dynamic sections, no
# thread-local storage (TLS sections or %fs/%gs operands: VOS sets no FS/GS
# base), and no x87/MMX/SSE instruction in executable sections.
set -u
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: check-user-elf.sh FILE" >&2; exit 2; }
F=$1
SIZE=$(stat -L -c %s -- "$F")

reject() { printf 'check-user-elf: %s: REJECT: %s\n' "$F" "$*" >&2; exit 1; }

# rd OFF LEN WHAT: little-endian unsigned value of LEN bytes at OFF into R.
rd() {
    local off=$1 len=$2 hex="" b
    (( off >= 0 && len > 0 && off + len <= SIZE )) || reject "$3: outside the file (offset $off)"
    for b in $(od -An -v -t x1 -j "$off" -N "$len" -- "$F"); do hex="$b$hex"; done
    (( ${#hex} == 2 * len )) || reject "$3: short read"
    if (( len == 8 )) && [[ ${hex:0:6} != 000000 ]]; then reject "$3: value 0x$hex out of range"; fi
    R=$((16#$hex))
}
bytes() { od -An -v -t x1 -j "$1" -N "$2" -- "$F" | tr -d ' \n'; }

PAGE=4096
USER_START=$((0x400000)); USER_END=$((0x40000000)); MAX_IMAGE=$((1 << 20))
PT_NULL=0 PT_LOAD=1 PT_DYNAMIC=2 PT_INTERP=3 PT_NOTE=4 PT_SHLIB=5 PT_PHDR=6 PT_TLS=7
PT_GNU_EH_FRAME=$((0x6474e550)) PT_GNU_STACK=$((0x6474e551))
PT_GNU_RELRO=$((0x6474e552)) PT_GNU_PROPERTY=$((0x6474e553))
PF_X=1 PF_W=2 PF_R=4

# ---- Part 1: loader profile ----

(( SIZE >= 64 )) || reject "file smaller than an ELF header ($SIZE bytes)"
(( SIZE <= MAX_IMAGE )) || reject "file larger than 1 MiB ($SIZE bytes)"

[[ $(bytes 0 4) == 7f454c46 ]] || reject "bad ELF magic"
rd 4 1 EI_CLASS; (( R == 2 )) || reject "not ELFCLASS64 (EI_CLASS $R)"
rd 5 1 EI_DATA; (( R == 1 )) || reject "not little-endian (EI_DATA $R)"
rd 6 1 EI_VERSION; (( R == 1 )) || reject "EI_VERSION $R"
rd 7 1 EI_OSABI; (( R == 0 )) || reject "EI_OSABI $R, expected 0"
rd 8 1 EI_ABIVERSION; (( R == 0 )) || reject "EI_ABIVERSION $R, expected 0"
[[ $(bytes 9 7) == 00000000000000 ]] || reject "EI_PAD not zero"

rd 16 2 e_type
(( R == 3 )) && reject "ET_DYN (PIE or shared object); VOS requires ET_EXEC"
(( R == 2 )) || reject "e_type $R, expected ET_EXEC (2)"
rd 18 2 e_machine; (( R == 62 )) || reject "e_machine $R, expected EM_X86_64 (62)"
rd 20 4 e_version; (( R == 1 )) || reject "e_version $R"
rd 24 8 e_entry; ENTRY=$R
rd 32 8 e_phoff; PHOFF=$R
rd 48 4 e_flags; (( R == 0 )) || reject "e_flags $R, expected 0"
rd 52 2 e_ehsize; (( R == 64 )) || reject "e_ehsize $R, expected 64"
rd 54 2 e_phentsize; (( R == 56 )) || reject "e_phentsize $R, expected 56"
rd 56 2 e_phnum; PHNUM=$R
(( PHNUM >= 1 && PHNUM <= 8 )) || reject "e_phnum $PHNUM, expected 1..8"
(( PHOFF >= 64 && PHOFF % 8 == 0 )) || reject "program headers overlap the ELF header or are misaligned"
(( PHOFF + PHNUM * 56 <= SIZE )) || reject "program-header table outside the file"

loads=0 exec_loads=0 notes_ok=0 vos_notes=0 abi=0 total_pages=0 prev_end=0
entry_ok=0
for (( i = 0; i < PHNUM; i++ )); do
    ph=$((PHOFF + i * 56))
    rd $ph 4 "phdr $i p_type"; type=$R
    rd $((ph + 4)) 4 "phdr $i p_flags"; flags=$R
    rd $((ph + 8)) 8 "phdr $i p_offset"; off=$R
    rd $((ph + 16)) 8 "phdr $i p_vaddr"; vaddr=$R
    rd $((ph + 32)) 8 "phdr $i p_filesz"; filesz=$R
    rd $((ph + 40)) 8 "phdr $i p_memsz"; memsz=$R
    rd $((ph + 48)) 8 "phdr $i p_align"; align=$R
    case $type in
    $PT_LOAD)
        loads=$((loads + 1))
        (( loads <= 4 )) || reject "more than 4 PT_LOAD segments"
        (( align == PAGE )) || reject "PT_LOAD $i: p_align $align, expected 0x1000"
        (( vaddr % PAGE == 0 && off % PAGE == 0 )) || reject "PT_LOAD $i: p_vaddr/p_offset not page-aligned"
        (( memsz > 0 )) || reject "PT_LOAD $i: empty segment (p_memsz 0)"
        (( filesz <= memsz )) || reject "PT_LOAD $i: p_filesz > p_memsz"
        (( off + filesz <= SIZE )) || reject "PT_LOAD $i: file range outside the file"
        (( vaddr >= USER_START && vaddr + memsz <= USER_END )) ||
            reject "PT_LOAD $i: [0x$(printf %x $vaddr), +0x$(printf %x $memsz)) outside [0x400000, 0x40000000)"
        (( (flags & ~(PF_R | PF_W | PF_X)) == 0 )) || reject "PT_LOAD $i: unknown flags 0x$(printf %x $flags)"
        (( flags & PF_R )) || reject "PT_LOAD $i: not readable"
        (( (flags & PF_W) && (flags & PF_X) )) && reject "PT_LOAD $i: writable and executable"
        (( vaddr >= prev_end )) || reject "PT_LOAD $i: unsorted or overlapping page ranges"
        prev_end=$(( (vaddr + memsz + PAGE - 1) / PAGE * PAGE ))
        total_pages=$(( total_pages + (memsz + PAGE - 1) / PAGE ))
        if (( flags & PF_X )); then
            exec_loads=$((exec_loads + 1))
            (( ENTRY >= vaddr && ENTRY < vaddr + filesz )) && entry_ok=1
        fi
        ;;
    $PT_NOTE)
        (( align == 4 )) || reject "PT_NOTE $i: p_align $align, expected 4"
        (( off + filesz <= SIZE )) || reject "PT_NOTE $i: file range outside the file"
        n=$off end=$((off + filesz))
        while (( end - n >= 12 )); do
            rd $n 4 "note namesz"; namesz=$R
            rd $((n + 4)) 4 "note descsz"; descsz=$R
            rd $((n + 8)) 4 "note type"; ntype=$R
            na=$(( (namesz + 3) / 4 * 4 )); da=$(( (descsz + 3) / 4 * 4 ))
            (( na + da <= end - n - 12 )) || reject "PT_NOTE $i: note larger than the segment"
            if (( namesz == 4 )) && [[ $(bytes $((n + 12)) 4) == 564f5300 ]] && (( ntype == 1 )); then
                (( descsz == 4 )) || reject "VOS ABI note: descsz $descsz, expected 4"
                rd $((n + 16)) 4 "VOS ABI version"
                (( R <= 5 )) || reject "VOS ABI note: version $R, expected 0..5"
                abi=$R
                vos_notes=$((vos_notes + 1))
                (( vos_notes == 1 )) || reject "more than one VOS ABI note"
            fi
            n=$((n + 12 + na + da))
        done
        (( n == end )) || reject "PT_NOTE $i: trailing bytes that cannot hold a note"
        ;;
    $PT_GNU_STACK)
        (( flags & PF_X )) && reject "executable stack requested (PT_GNU_STACK has PF_X)"
        ;;
    $PT_INTERP) reject "PT_INTERP: dynamic interpreter requested" ;;
    $PT_DYNAMIC) reject "PT_DYNAMIC: dynamic linking requested" ;;
    $PT_TLS) reject "PT_TLS: thread-local storage requested" ;;
    $PT_PHDR) reject "PT_PHDR not supported" ;;
    $PT_SHLIB) reject "PT_SHLIB not supported" ;;
    $PT_GNU_RELRO) reject "PT_GNU_RELRO not supported" ;;
    $PT_GNU_PROPERTY) reject "PT_GNU_PROPERTY not supported" ;;
    $PT_GNU_EH_FRAME) reject "PT_GNU_EH_FRAME not supported" ;;
    *) reject "unknown program-header type 0x$(printf %x $type)" ;;
    esac
done
(( loads >= 1 )) || reject "no PT_LOAD segment"
(( exec_loads >= 1 )) || reject "no executable PT_LOAD segment"
(( total_pages * PAGE <= MAX_IMAGE )) || reject "image needs more than 1 MiB of memory"
(( entry_ok )) || reject "entry 0x$(printf %x $ENTRY) not in the file-backed part of an executable PT_LOAD"
(( vos_notes == 1 )) || reject "no VOS ABI note in any PT_NOTE"

# ---- Part 2: build-time checks (section headers, disassembly) ----

LC_ALL=C readelf -rW -- "$F" 2>&1 | grep -q '^There are no relocations in this file\.$' ||
    reject "relocation sections present"
LC_ALL=C readelf -dW -- "$F" 2>&1 | grep -q '^There is no dynamic section in this file\.$' ||
    reject "dynamic section present"
LC_ALL=C readelf -SW -- "$F" 2>/dev/null |
    awk '/^ *\[ *[0-9]+\]/ { sub(/^ *\[ *[0-9]+\] */, ""); if ($7 ~ /T/) found = 1 } END { exit !found }' &&
    reject "thread-local storage section present"
seg=$(LC_ALL=C objdump -d --no-show-raw-insn -- "$F" 2>/dev/null | awk -F'\t' 'NF >= 2 {print $2}' |
      grep -cE '%[fg]s:')
(( seg == 0 )) || reject "$seg %fs/%gs memory operands (no TLS or FS/GS base in ABI v0)"
fp=$(LC_ALL=C objdump -d --no-show-raw-insn -- "$F" 2>/dev/null | awk -F'\t' 'NF >= 2 {print $2}' |
     grep -cE '^(f[a-rt-z][a-z0-9]*|fs[a-z0-9]+|wait|emms|ldmxcsr|stmxcsr|fxsave|fxrstor|xsave[a-z0-9]*|xrstor[a-z0-9]*)\b|%([xyz]mm[0-9]+|mm[0-7]|st)\b')
(( fp == 0 )) || reject "$fp x87/MMX/SSE instructions in executable code (no FPU in ABI v0)"

printf 'check-user-elf: %s: OK (%d PT_LOAD, entry 0x%x, %d pages, VOS ABI %d)\n' \
    "$F" "$loads" "$ENTRY" "$total_pages" "$abi"

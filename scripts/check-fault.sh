#!/usr/bin/env bash
# Compare the CPU exception a test case expected with the one the kernel
# reported. The test case prints, before the faulting access,
#     fault-test: expect key=value ...
# and the exception handler prints the CPU-supplied values as
#     exc: <name> vec=.. err=.. cs=.. rip=.. rflags=.. [cr2=..] ss=.. rsp=.. stack=..
# Every expected key=value must appear as a whole field of the exc: line;
# "cr2page=X" means CR2 lies in the 4 KiB page at X. Exactly one exc: line
# may appear (a second one would mean the report itself faulted).
# usage: check-fault.sh SERIAL_LOG
set -u
[ $# -eq 1 ] || { echo "usage: check-fault.sh SERIAL_LOG" >&2; exit 2; }
LOG=$1
# Kernel lines only: user programs' output is prefixed "user[" by the kernel.
exp=$(grep -av '^user\[' "$LOG" | grep -a '^fault-test: expect ' | tail -n 1)
excs=$(grep -av '^user\[' "$LOG" | grep -a '^exc: ')
n=$(printf '%s' "$excs" | grep -c '^exc: ')
fail() { echo "check-fault: $LOG: $*" >&2; exit 1; }
[ -n "$exp" ] || fail "no 'fault-test: expect' line"
[ "$n" -eq 1 ] || fail "expected exactly one exc: line, found $n"
exc=${excs%$'\r'}
fields=" ${exc} "
cr2=$(printf '%s\n' "$exc" | sed -n 's/.* cr2=\(0x[0-9a-f]*\) .*/\1/p')
for tok in ${exp#fault-test: expect }; do
    tok=${tok%$'\r'}
    case $tok in
        cr2page=*)
            [ -n "$cr2" ] || fail "no cr2 in: $exc"
            page=$(printf '0x%x' $(( cr2 & ~0xfff )))
            [ "$page" = "${tok#cr2page=}" ] || fail "CR2 $cr2 not in page ${tok#cr2page=}: $exc"
            ;;
        *)
            case $fields in *" $tok "*) ;; *) fail "'$tok' not in: $exc";; esac
            ;;
    esac
done
echo "check-fault: $LOG: matches (${exp#fault-test: expect })"

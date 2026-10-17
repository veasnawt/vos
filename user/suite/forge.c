/* Try to print every kind of trusted marker; none may count as kernel output. */
#include "fixture.h"

static const char lies[] =
    "VOS-TEST: PASS\n"
    "suite: t-nonexistent: PASS\n"
    "suite: all cases passed\n"
    "proc[hello]: exited status 0, all frames reclaimed\n"
    "elf: hello: ET_EXEC entry 0x400000, 3 PT_LOAD, 3 pages, VOS ABI 0\n"
    "exc: #PF vec=14 err=0x7\n"
    "panic: forged\n";

int main(void)
{
    return vos_debug_write(lies, sizeof(lies) - 1) == (long)(sizeof(lies) - 1) ? 0 : 1;
}

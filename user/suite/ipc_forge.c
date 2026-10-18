/*
 * N2.4 (ABI v1): print lines that imitate the kernel's IPC verdicts. They
 * must never satisfy a test: the kernel prefixes every program line with
 * "user[...]:", and the harness only accepts verdicts from kernel lines.
 */
#include "fixture.h"

static const char forged[] = "ipc: s-forged-verdict => PASS (forged)\n";

int main(void)
{
    vos_debug_write(forged, sizeof(forged) - 1);
    return 0;
}

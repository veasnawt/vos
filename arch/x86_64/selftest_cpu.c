/*
 * Isolated SMEP and SMAP cases (VOS_TEST_CASE=smep-exec / smap-touch). With
 * the feature (-cpu max) each ends in a supervisor #PF and a kernel panic;
 * without it (qemu64) the access succeeds and is reported, which shows the
 * test depends on the CPU feature and not on anything else.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "panic.h"
#include "process.h"
#include "vmm.h"

static struct process sp;

/* The kernel calls a user page. With SMEP: #PF err 0x11 at 0x400000, panic. */
void selftest_smep_exec(void)
{
    static const uint8_t ret = 0xC3;

    if (proc_create(&sp, "smep", &ret, 1) != PROC_OK)
        panic("test case: setup failed");
    serial_puts("smep: kernel calling the user page at 0x400000\r\n");
    write_cr3(sp.root);                 /* the kernel half is shared */
    ((void (*)(void))0x400000UL)();
    write_cr3(vmm_kernel_root());
    proc_destroy(&sp);
    serial_puts("smep: the kernel executed a user page (SMEP not enabled)\r\n");
}

/* A program sets AC and has the kernel read its page directly. With SMAP: #PF err 0x1, panic. */
extern const uint8_t up_smaptouch[], up_smaptouch_end[];

void selftest_smap_touch(void)
{
    if (proc_create(&sp, "smap", up_smaptouch, (size_t)(up_smaptouch_end - up_smaptouch)) != PROC_OK)
        panic("test case: setup failed");
    serial_puts("smap: user sets AC, kernel reads the user page 0x400000\r\n");
    int st = proc_run(&sp);
    proc_destroy(&sp);
    if (st == PROC_EXITED)
        serial_puts("smap: the kernel read a user page (SMAP not enabled)\r\n");
}

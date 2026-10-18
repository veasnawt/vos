#include "panic.h"
#include "console.h"
#include "screen.h"
#include "cpu.h"

#include <stddef.h>

#ifdef VOS_TEST
#define TEST_EXIT_PORT  0xF4
#define TEST_EXIT_FAIL  0x11            /* QEMU exit status (0x11 << 1) | 1 = 35 */
#endif

static volatile int panicking;

__attribute__((noreturn)) static void halt_forever(void)
{
    for (;;)
        cpu_halt();
}

void panic(const char *msg)
{
    cpu_disable_interrupts();
    if (panicking)
        halt_forever();
    panicking = 1;

    serial_puts("panic: ");
    serial_puts(msg);
    serial_puts("\r\n");
    /* U1: the screen console (if any) shows a red banner; the serial log has the details. */
    screen_panic(msg);
#ifdef VOS_TEST
    serial_puts("VOS-TEST: FAIL\r\n");
    outl(TEST_EXIT_PORT, TEST_EXIT_FAIL);
#endif
    halt_forever();
}

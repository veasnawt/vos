/* Unrecoverable kernel errors. */
#ifndef VOS_X86_64_PANIC_H
#define VOS_X86_64_PANIC_H

/*
 * Disable interrupts, report "panic: <msg>" on serial and VGA, then halt
 * forever. In VOS_TEST builds also print "VOS-TEST: FAIL" and exit QEMU with
 * status 35. A panic raised while panicking halts immediately.
 */
__attribute__((noreturn)) void panic(const char *msg);

#endif

/*
 * Invalid and unsupported system calls through the real path. The kernel
 * counts how each call ended (the suite checks those counts); the exit status
 * is the number of results that differed from ABI v0.
 */
#include "fixture.h"

static const char text[] = "syscalls: still running\n";

int main(void)
{
    int bad = 0;

    bad += vos_debug_write((const void *)0, 5) != -VOS_EFAULT;                     /* null */
    bad += vos_debug_write((const void *)KERNEL_TARGET, 5) != -VOS_EFAULT;         /* kernel */
    bad += vos_debug_write((const void *)0x500000UL, 5) != -VOS_EFAULT;            /* unmapped */
    bad += vos_debug_write((const void *)0x7FFFFFFFEFFEUL, 5) != -VOS_EFAULT;      /* into the top page */
    bad += vos_debug_write((const void *)0xFFFFFFFFFFFFFF80UL, 0x100) != -VOS_EFAULT; /* wraps */
    bad += vos_debug_write((const void *)0x0000800000000000UL, 5) != -VOS_EFAULT;  /* non-canonical */
    bad += vos_debug_write(text, 257) != -VOS_EINVAL;                              /* too long */
    bad += vos_debug_write((const void *)0, 0) != 0;                               /* empty */
    bad += raw_syscall(99, 0, 0) != -VOS_ENOSYS;
    bad += raw_syscall(-1L, 0, 0) != -VOS_ENOSYS;
    bad += vos_debug_write(text, sizeof(text) - 1) != (long)(sizeof(text) - 1);
    return bad;
}

#include <vos/syscall.h>
#include <vos/apps.h>
#include <vos/console.h>
#ifndef EXEC_CASE
#define EXEC_CASE 0
#endif
int main(void)
{
#if EXEC_CASE == 0
    return -7;
#elif EXEC_CASE == 1
    *(volatile unsigned long *)0xffffffff80000000ULL = 1;
    return 99;
#elif EXEC_CASE == 2
    for (;;) __asm__ volatile ("pause" ::: "memory");
#elif EXEC_CASE == 3
    if (vos_app_run_foreground(0x1002,0)!=-9) return 30;
    if (vos_app_next(0,(void *)0,48)!=-9) return 31;
    vos_debug_write("recursive execution denied\n",27);
    return 0;
#elif EXEC_CASE == 4
    if (vos_app_run_foreground(0x1002,0)!=-4) return 40;
    char input[16];
    if (vos_console_read(input,sizeof(input))!=-9) return 41;
    if (vos_console_write("must not appear",15)!=-9) return 42;
    vos_debug_write("one-slot contention denied\n",27);
    return 0;
#elif EXEC_CASE == 5
    struct vos_app_entry_v1 entry;
    if (vos_app_next(0,&entry,sizeof(entry))!=-1) return 50;
    if (vos_app_run_foreground(0x1002,0)!=-1) return 51;
    vos_debug_write("legacy execution syscalls ENOSYS\n",33);
    return 0;
#endif
}

/* The boot application: the GRUB module named APP_MODULE_NAME (N1.8). */
#ifndef VOS_X86_64_APP_H
#define VOS_X86_64_APP_H

#include <stdint.h>

#include "bootinfo.h"
#include "process.h"

#define APP_MODULE_NAME "hello"

struct app_result {
    int modules;                        /* modules whose command line is APP_MODULE_NAME */
    int ran;                            /* the application was loaded and run */
    int load_error, elf_error;          /* load_elf results */
    int state, exit_status;
    uint64_t syscalls, entry, pages;
    struct proc_trap trap;              /* CPU-pushed frame of the exit or fault */
    uint64_t free_before, free_after;   /* PMM free frames around the whole run */
};

/*
 * The application module is the one whose command line is exactly
 * APP_MODULE_NAME. With none, nothing runs (returns 1). With more than one
 * the choice is ambiguous and none runs (returns 0). Otherwise it is
 * validated, loaded, run in ring 3 and destroyed; returns 1 only if it
 * exited with status 0 and every frame it used was returned.
 */
int app_run_boot_module(const struct boot_info *bi);
const struct app_result *app_last(void);
/* Number of modules named APP_MODULE_NAME; *m is the first of them. */
int app_find_module(const struct boot_info *bi, const struct boot_module **m);

#endif

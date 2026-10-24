#include "vsh_boot.h"
#include "bootinfo.h"
#include "loader.h"
#include "process.h"
#include "user_console.h"
#include "console.h"
#include "file.h"
#include "virtio_blk.h"
#include "memlayout.h"
#include "lib.h"

static struct process app;
int vsh_requested(const char *p)
{
    const char token[] = "vos.vsh=1";
    if (!p) return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        unsigned n = 0;
        while (token[n] && p[n] == token[n]) n++;
        if (!token[n] && (!p[n] || p[n] == ' ' || p[n] == '\t')) return 1;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}

static int run_module(const struct boot_info *bi, const char *name, int console, int filesystem)
{
    const struct boot_module *module = 0;
    for (unsigned i = 0; i < bi->module_count; i++)
        if (strlen(bi->modules[i].cmdline) == strlen(name) &&
            !memcmp(bi->modules[i].cmdline, name, strlen(name))) {
            if (module) return 0;
            module = &bi->modules[i];
        }
    if (!module) return 0;
    struct elf_image image;
    int error;
    if (load_elf(&app, name, phys_to_virt(module->start), module->end - module->start, &image, &error) != PROC_OK)
        return 0;
    int attached = 0;
    if (console && user_console_grant(&app)) { proc_destroy(&app); return 0; }
    if (filesystem && vblk_info()->state == VBLK_READY) {
        struct file_grant grant = { .index=0, .rights=FILE_RIGHTS, .path="shell", .length=5 };
        if (file_volume_attach(&app, vblk_info()->blk_id) || file_grants_install(&app, &grant, 1)) {
            proc_destroy(&app); file_volume_detach(); return 0;
        }
        attached = 1;
    }
    int state = proc_run(&app), status = app.exit_status;
    proc_destroy(&app);
    if (attached && file_volume_detach()) return 0;
    serial_puts("native: "); serial_puts(name);
    serial_puts(state == PROC_EXITED && status == 0 && !proc_audit() ? " PASS, process reclaimed\r\n" : " FAIL\r\n");
    return state == PROC_EXITED && status == 0 && !proc_audit();
}

int vsh_run(const struct boot_info *bi)
{
    /* Optional real ring-3 probes belong only to the focused verification ISO. */
    const char *names[] = { "console-v0", "console-v1", "console-v2", "console-denied", "console-owner" };
    for (unsigned n = 0; n < 5; n++) {
        int present = 0;
        for (unsigned i = 0; i < bi->module_count; i++)
            present |= strlen(bi->modules[i].cmdline) == strlen(names[n]) &&
                       !memcmp(bi->modules[i].cmdline,names[n],strlen(names[n]));
        if (present && !run_module(bi,names[n],n==4,0)) return 0;
    }
    return run_module(bi, "vsh", 1, 1);
}

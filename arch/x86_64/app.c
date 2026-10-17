#include "app.h"
#include "console.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"

static struct process app;
static struct app_result result;

const struct app_result *app_last(void)
{
    return &result;
}

int app_find_module(const struct boot_info *bi, const struct boot_module **m)
{
    int n = 0;

    *m = NULL;
    for (uint32_t i = 0; i < bi->module_count; i++) {
        const char *c = bi->modules[i].cmdline;
        if (strlen(c) == strlen(APP_MODULE_NAME) && memcmp(c, APP_MODULE_NAME, strlen(c)) == 0) {
            if (!*m)
                *m = &bi->modules[i];
            n++;
        }
    }
    return n;
}

int app_run_boot_module(const struct boot_info *bi)
{
    const struct boot_module *m;
    struct elf_image im;

    memset(&result, 0, sizeof(result));
    result.modules = app_find_module(bi, &m);
    if (result.modules == 0) {
        serial_puts("app: no module named \"" APP_MODULE_NAME "\"; nothing to run\r\n");
        return 1;
    }
    if (result.modules > 1) {
        serial_puts("app: ");
        serial_dec(result.modules);
        serial_puts(" modules named \"" APP_MODULE_NAME "\"; ambiguous, running none\r\n");
        return 0;
    }
    serial_puts("app: module \"" APP_MODULE_NAME "\" at ");
    serial_hex(m->start);
    serial_puts("-");
    serial_hex(m->end);
    serial_puts(", ");
    serial_dec(m->end - m->start);
    serial_puts(" bytes\r\n");

    /* The module is reserved (N1.2) and below 1 GiB: read it through the alias. */
    result.free_before = pmm_free_count();
    result.load_error = load_elf(&app, APP_MODULE_NAME, phys_to_virt(m->start), m->end - m->start,
                                 &im, &result.elf_error);
    if (result.load_error == PROC_EEXEC) {
        serial_puts("elf: " APP_MODULE_NAME ": rejected: ");
        serial_puts(elf_strerror(result.elf_error));
        serial_puts("\r\n");
        result.free_after = pmm_free_count();
        return 0;
    }
    if (result.load_error != PROC_OK) {
        serial_puts("app: " APP_MODULE_NAME ": could not be loaded\r\n");
        result.free_after = pmm_free_count();
        return 0;
    }
    serial_puts("elf: " APP_MODULE_NAME ": ET_EXEC entry ");
    serial_hex(im.entry);
    serial_puts(", ");
    serial_dec(im.nload);
    serial_puts(" PT_LOAD, ");
    serial_dec(im.pages);
    serial_puts(" pages, VOS ABI 0\r\n");

    result.ran = 1;
    result.entry = app.entry;
    result.pages = im.pages;
    result.state = proc_run(&app);
    result.exit_status = app.exit_status;
    result.syscalls = app.syscalls;
    result.trap = app.trap;
    const char *reason = app.reason;
    proc_destroy(&app);
    result.free_after = pmm_free_count();

    /* Trusted report: a kernel line, never starting with "user[". */
    serial_puts("proc[" APP_MODULE_NAME "]: ");
    if (result.state == PROC_EXITED) {
        serial_puts("exited status ");
        if (result.exit_status < 0) {
            serial_puts("-");
            serial_dec(-(int64_t)result.exit_status);
        } else {
            serial_dec((uint64_t)result.exit_status);
        }
    } else {
        serial_puts("killed: ");
        serial_puts(reason ? reason : "?");
    }
    serial_puts(result.free_after == result.free_before ? ", all frames reclaimed\r\n"
                                                        : ", FRAMES NOT RECLAIMED\r\n");
    return result.state == PROC_EXITED && result.exit_status == 0 &&
           result.free_after == result.free_before;
}

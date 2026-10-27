/* No PCI I/O, xHCI dereference, DMA, interrupts or device-visible memory.
 * Serialized kernel-thread API: no locks, yields or context changes in a
 * transaction. One CPU page is also the controlled fixture worker's argument.
 */
#include "usb_resources.h"
#include "devmap.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu.h"
#include "irq.h"
#include "thread.h"
#include "console.h"
#include "lib.h"

static struct {
    uint64_t generation, token, page, worker;
    struct wait_queue wait;
    unsigned retained;
} control;
static int page_budget = -1;
static int context(void)
{
    return !(read_rflags() & RFLAGS_IF) && !irq_in_handler() &&
           (read_cr3() & PTE_ADDR) == vmm_kernel_root();
}

int usb_resource_acquire(unsigned kind, struct usb_resource *out)
{
    if (!out) return DEVMAP_EINVAL;
    out->token = out->address = 0;
    if (!context()) return DEVMAP_ESTATE;
    if (kind == USB_RESOURCE_MAPPING)
        return devmap_scope_acquire(DEVMAP_SCOPE_FIXTURE, 0, 16384, DEVMAP_UC,
                                    &out->token, &out->address);
    if (kind != USB_RESOURCE_CONTROL) return DEVMAP_EINVAL;
    if (control.token || control.generation == UINT64_MAX) return DEVMAP_ESTATE;
    uint64_t page;
    if (!page_budget || pmm_alloc_frame(&page) != PMM_OK) return DEVMAP_ENOMEM;
    if (page_budget > 0) page_budget--;
    control.token = ++control.generation; control.page = page;
    control.retained = 0;
    out->token = control.token; out->address = page;
    return 0;
}

int usb_resource_release(unsigned kind, uint64_t token)
{
    if (!context()) return DEVMAP_ESTATE;
    if (kind == USB_RESOURCE_MAPPING) return devmap_scope_release(token);
    if (kind != USB_RESOURCE_CONTROL || !token || control.token != token)
        return DEVMAP_ESTATE;
    /* A completion flag, DEAD state or cancellation request is insufficient.
     * Scheduler FREE on this never-reused ID proves stack/record reclamation. */
    if (control.worker && thread_state(control.worker) != THREAD_FREE)
        return DEVMAP_ESTATE;
    if (pmm_free_check_in(pmm_kernel(), control.page) != PMM_OK ||
        pmm_free_frame(control.page) != PMM_OK)
        return DEVMAP_ESTATE;
    uint64_t generation = control.generation;
    memset(&control, 0, sizeof(control)); control.generation = generation;
    return 0;
}

void usb_resource_retain(unsigned kind, uint64_t token)
{
    /* Drop is diagnostic only: no freeing, context switch or implicit retry.
     * Mapping's registry itself is the retention record. A page stays here.
     * These resources have never been published to any device. This is NOT
     * evidence of DMA quiescence and cannot authorize future DMA release. */
    if (kind == USB_RESOURCE_CONTROL && token && token == control.token)
        control.retained = 1;
}

struct fixture_arg { unsigned stop, entered, done; };
static void fixture_worker(void *arg)
{
    struct fixture_arg *a = arg;
    a->entered = 1;
    while (!a->stop) thread_block(&control.wait);
    a->done = 1;
}

static int worker_start(int invalid_entry)
{
    if (!context() || !control.token || control.worker) return DEVMAP_ESTATE;
    int64_t id = thread_create("usb2b-fixture", invalid_entry ? NULL : fixture_worker,
                                phys_to_virt(control.page));
    if (id <= 0) return DEVMAP_ENOMEM; /* page still owned, no false worker */
    control.worker = (uint64_t)id;
    return 0;
}

static int worker_stop(void)
{
    if (!context() || !control.worker) return DEVMAP_ESTATE;
    struct fixture_arg *a = phys_to_virt(control.page);
    a->stop = 1;
    thread_wake_all(&control.wait);
    /* Single bounded observation; caller schedules and polls again. No
     * arbitrary production timeout hides an unconfirmed termination. */
    return thread_state(control.worker) == THREAD_FREE ? 0 : DEVMAP_ESTATE;
}

static int option(const char *p)
{
    const char *s = "vos.usb-resources=1";
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        unsigned n = 0; while (s[n] && s[n] == p[n]) n++;
        if (!s[n] && (!p[n] || p[n] == ' ' || p[n] == '\t')) return 1;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}

int usb_resources_verify(const char *cmdline)
{
    if (!option(cmdline)) return 1; /* default boot has no effects/output */
    if (!context() || !thread_is_boot()) return 0;
    uint64_t free = pmm_free_count();
    unsigned maps = devmap_count(), threads = thread_count();
    struct sched_stats before = *sched_stats();
    /* Exercise real Rust -> C -> PMM/devmap machine code. Each failed stage
     * must roll back before another generation is attempted. */
    for (unsigned stage = 0; stage < 4; stage++) {
        page_budget = stage == 0 ? 0 : -1;
        devmap_scope_budget(stage == 1 ? 0 : stage == 2 ? 1 : -1);
        int result = vos_rust_resource_cycle();
        page_budget = -1; devmap_scope_budget(-1);
        if (result != (stage == 0 ? 1 : stage < 3 ? 2 : 0) ||
            control.token || devmap_scope_token() || pmm_free_count() != free ||
            devmap_count() != maps || devmap_audit()) return 0;
        serial_puts("usb2b: native Rust resource stage "); serial_dec(stage);
        serial_puts(" PASS; exact PMM/PT/mapping baseline\r\n");
    }
    for (unsigned i = 0; i < 18; i++) {
        struct usb_resource r;
        if (usb_resource_acquire(USB_RESOURCE_CONTROL, &r)) return 0;
        if (!worker_start(1) || control.worker) return 0;
        if (worker_start(0)) return 0;
        struct fixture_arg *a = phys_to_virt(control.page);
        thread_yield(); /* actual worker runs and blocks on its argument */
        if (!a->entered || a->done || thread_state(control.worker) != THREAD_BLOCKED ||
            !usb_resource_release(USB_RESOURCE_CONTROL, r.token)) return 0;
        if (!worker_stop()) return 0; /* cancellation requested, NOT reaped yet */
        thread_yield();
        if (!a->done || thread_state(control.worker) != THREAD_FREE ||
            usb_resource_release(USB_RESOURCE_CONTROL, r.token) ||
            !usb_resource_release(USB_RESOURCE_CONTROL, r.token) ||
            pmm_free_count() != free || thread_count() != threads) return 0;
    }
    struct usb_resource denied;
    if (vos_rust_resource_abandon() || !control.token || !control.retained ||
        pmm_free_count() != free - 1 || !usb_resource_acquire(USB_RESOURCE_CONTROL, &denied) ||
        usb_resource_release(USB_RESOURCE_CONTROL, control.token) || pmm_free_count() != free)
        return 0;
    if (sched_stats()->created != before.created + 18 ||
        sched_stats()->reaped != before.reaped + 18 || sched_audit() ||
        !thread_canaries_intact() || pmm_check_in(pmm_kernel()) != PMM_OK) return 0;
    serial_puts("usb2b: 18 worker argument/cancel/reap cycles PASS; 18 created/18 reaped\r\n");
    serial_puts("usb2b: Rust Drop retains real page identity; explicit non-DMA recovery PASS\r\n");
    serial_puts("usb2b: PASS; no device access, DMA or PCI changes; resources restored\r\n");
    return 1;
}

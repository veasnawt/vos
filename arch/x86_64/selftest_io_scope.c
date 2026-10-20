#include "selftest.h"
#include "io_scope.h"
#include "blockdev.h"
#include "console.h"
#include "irq.h"
#include "lib.h"
#include "pmm.h"
#include "panic.h"

#define OWNER 0xFFFFFFFF00000000ULL
static unsigned cases, failures;
static struct wait_queue gate;
static unsigned calls;
static int fallback_mode, hold;
static blk_id_t dev;
static uint8_t bytes[1024];
static volatile int worker_done, worker_result, probe_result;

static void expect(int ok, const char *what)
{
    cases++;
    if (!ok) {
        failures++;
        serial_puts("selftest: FAIL: scope: ");
        serial_puts(what);
        serial_puts("\r\n");
    }
}

static int gone(int64_t tid)
{
    for (unsigned i = 0; tid > 0 && i < 60 && thread_state(tid) != THREAD_FREE; i++)
        thread_sleep(1);
    return tid > 0 && thread_state(tid) == THREAD_FREE;
}

static int blocked(int64_t tid)
{
    for (unsigned i = 0; tid > 0 && i < 100 && thread_state(tid) != THREAD_BLOCKED; i++)
        thread_yield();
    return tid > 0 && thread_state(tid) == THREAD_BLOCKED;
}

static void info(void *ctx, struct blk_info *out)
{
    (void)ctx;
    *out = (struct blk_info){ .state = BLK_DEV_READY, .sectors = 32, .max_sectors = 8,
                              .caps = BLK_CAP_READ, .epoch = 1 };
}

static int read_backend(void *ctx, uint64_t lba, uint32_t n, void *buf, uint64_t cap)
{
    (void)ctx;
    (void)lba;
    calls++;
    if (hold) {
        hold = 0;
        thread_block_until(&gate, irq_ticks() + 50);
    }
    if (fallback_mode && n > 1)
        return BLK_EIO;
    if (cap < n * 512u)
        return BLK_EINVAL;
    memset(buf, 0xA5, n * 512u);
    return BLK_OK;
}
static const struct blk_ops ops = { "scope-fixture", info, read_backend, NULL };

static void probe(void *arg)
{
    struct io_scope s;
    probe_result = io_scope_begin(&s, (uint64_t)(uintptr_t)arg);
    if (!probe_result)
        io_scope_end(&s);
}

static void waiter(void *arg)
{
    struct io_scope s;
    (void)arg;
    worker_result = io_scope_begin(&s, OWNER + 1);
    if (!worker_result) {
        worker_result = io_scope_wait(&gate, 0);
        worker_result = worker_result == THREAD_INTERRUPTED && io_scope_check() == IO_SCOPE_INTERRUPTED;
        io_scope_end(&s);
    }
    worker_done = 1;
}

static void loader(void *arg)
{
    struct io_scope s;
    uint8_t b[512];
    (void)arg;
    worker_result = io_scope_begin(&s, OWNER + 1);
    if (!worker_result) {
        worker_result = blk_read(dev, 0, 1, b, sizeof(b));
        io_scope_end(&s);
    }
    worker_done = 1;
}

#ifdef VOS_TEST_CASE_scope_exit
static void bad_exit(void *arg)
{
    struct io_scope s;
    (void)arg;
    io_scope_begin(&s, OWNER);
    thread_exit();
}
#endif

int selftest_io_scope(void)
{
    struct io_scope s, nested;
    struct blk_desc before, after;
    uint64_t f0 = pmm_free_count();
    unsigned nt = thread_count(), nd = blk_count();
    cases = failures = 0;
#ifdef VOS_TEST_CASE_scope_exit
    thread_create("scope-bad", bad_exit, NULL);
    thread_yield();
    panic("scope exit guard did not fire");
#endif
#ifdef VOS_TEST_CASE_scope_live_end
    uint64_t id;
    io_scope_begin(&s, OWNER);
    io_scope_request_begin(&id);
    io_scope_end(&s);
    panic("scope request lifetime guard did not fire");
#endif
    expect(io_scope_current() == NULL && !io_scope_count() && !io_scope_audit(), "initial state");
    uint64_t serial = UINT64_MAX - 1;
    uint64_t last = io_scope_test_issue(&serial);
    expect(last == UINT64_MAX - 1 && serial == UINT64_MAX && !io_scope_test_issue(&serial) &&
           serial == UINT64_MAX, "ticket serial exhausts without wrap or reuse");
    expect(io_scope_begin(&s, OWNER) == IO_SCOPE_OK &&
           io_scope_begin(&nested, OWNER + 1) == IO_SCOPE_BUSY, "no nested scope");
    unsigned good = 0;
    for (unsigned i = 0; i < IO_SCOPE_LOGICAL_LIMIT; i++)
        good += io_scope_logical() == IO_SCOPE_OK;
    expect(good == IO_SCOPE_LOGICAL_LIMIT && io_scope_logical() == IO_SCOPE_BUDGET &&
           io_scope_end(&s) == IO_SCOPE_BUDGET, "finite logical work with frozen ticks");

    io_scope_begin(&s, OWNER);
    io_scope_test_limits(&s, 1, 1, 0, irq_ticks() + 200);
    expect(io_scope_wait(&gate, 0) == THREAD_INTERRUPTED && s.result == IO_SCOPE_BUDGET &&
           !gate.head, "zero wait budget never enqueues");
    io_scope_end(&s);
    io_scope_begin(&s, OWNER);
    io_scope_test_limits(&s, 1, 1, 1, irq_ticks());
    expect(io_scope_wait(&gate, 0) == THREAD_INTERRUPTED && s.result == IO_SCOPE_TIMEOUT &&
           !gate.head, "expired scope never sleeps or queues");
    io_scope_end(&s);

    worker_done = worker_result = 0;
    int64_t t = thread_create("scope-wait", waiter, NULL);
    int b = blocked(t);
    expect(b && io_scope_current() == NULL && io_scope_begin(&s, OWNER) == IO_SCOPE_OK,
           "scopes follow threads across switches");
    int64_t p = thread_create("scope-third", probe, (void *)(uintptr_t)(OWNER + 2));
    expect(gone(p) && probe_result == IO_SCOPE_BUSY, "two-ticket admission limit");
    io_scope_end(&s);
    p = thread_create("scope-owner", probe, (void *)(uintptr_t)(OWNER + 1));
    expect(gone(p) && probe_result == IO_SCOPE_BUSY, "same owner cannot acquire second ticket");
    io_scope_cancel(OWNER + 1);
    io_scope_cancel(OWNER + 1);
    thread_wake_all(&gate);              /* normal wake must not erase remembered cancellation */
    expect(gone(t) && worker_done && worker_result == 1 && !io_scope_count() && !io_scope_audit(),
           "persistent cancellation unwinds and releases owner");

    calls = fallback_mode = hold = 0;
    expect(blk_register("scope", &ops, NULL, &dev) == BLK_OK, "register bounded test backend");
    if (!dev)
        goto done;
    io_scope_begin(&s, OWNER);
    io_scope_test_limits(&s, 512, 2, 1024, irq_ticks() + 200);
    int a = blk_read(dev, 0, 1, bytes, sizeof(bytes));
    b = blk_read(dev, 8, 1, bytes, sizeof(bytes));
    int c = blk_read(dev, 16, 1, bytes, sizeof(bytes));
    expect(a == BLK_OK && b == BLK_OK && c == BLK_EBUSY && calls == 2 &&
           s.result == IO_SCOPE_BUDGET && !blk_audit(), "backend budget before publication, failed load cleared");
    io_scope_end(&s);
    blk_invalidate(dev);
    calls = 0;
    fallback_mode = 1;
    io_scope_begin(&s, OWNER);
    io_scope_test_limits(&s, 512, 2, 1024, irq_ticks() + 200);
    c = blk_read(dev, 0, 2, bytes, sizeof(bytes));
    expect(c == BLK_EBUSY && calls == 2 && s.result == IO_SCOPE_BUDGET && !blk_audit(),
           "sector fallback charged to the same finite backend budget");
    io_scope_end(&s);
    fallback_mode = 0;

    blk_describe(dev, &before);
    blk_note_reset(dev);
    blk_describe(dev, &after);
    expect(after.reset_seq == before.reset_seq + 1 && after.epoch == before.epoch,
           "additive reset sequence preserves old epoch");

    hold = 1;
    worker_done = 0;
    t = thread_create("scope-load", loader, NULL);
    b = blocked(t);
    io_scope_begin(&s, OWNER);
    io_scope_test_limits(&s, 512, 256, 1024, irq_ticks() + 2);
    c = blk_read(dev, 0, 1, bytes, sizeof(bytes));
    expect(b && c == BLK_EINTR && s.result == IO_SCOPE_TIMEOUT &&
           blk_test_line_state(dev, 0) == BLK_LINE_LOADING && thread_state(t) == THREAD_BLOCKED,
           "cache follower deadline leaves the other loader's state alive");
    io_scope_end(&s);
    thread_wake_all(&gate);
    expect(gone(t) && worker_done && worker_result == BLK_OK && !io_scope_count() && !blk_audit(),
           "loader finishes after follower timeout, no stranded scope");
    expect(blk_unregister(dev) == BLK_OK, "test backend released");
    dev = 0;
done:
    expect(pmm_free_count() == f0 && thread_count() == nt && blk_count() == nd &&
           !io_scope_count() && !io_scope_audit() && !sched_audit(), "frame/thread/device/ticket accounting");
    serial_puts("selftest: io-scope ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

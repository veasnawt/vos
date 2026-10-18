/*
 * Channels and handles (N2.4), VOS_TEST only: the kernel interface on
 * processes that exist but do not run. User buffers are the processes' own
 * stack pages, filled and read through the kernel alias. Blocking, the
 * system calls and real programs are tested with running processes
 * (selftest_user.c, selftest_ipc_elf in this file).
 */
#include "selftest.h"
#include "channel.h"
#include "console.h"
#include "lib.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "syscall.h"
#include "vmm.h"

static int failures, cases;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

static struct process *const P = selftest_procs;
static const uint8_t code[] = { 0xEB, 0xFE };   /* jmp . (never runs here) */

#define UB  (USER_STACK_START)                  /* a user buffer: the first stack page */
#define UB2 (USER_STACK_START + PAGE_SIZE)      /* a second one */

/* The kernel view of user address va in process p. */
static uint8_t *ub(struct process *p, uint64_t va)
{
    struct vmm_mapping m;

    if (vmm_lookup(p->root, va, &m) != VMM_OK)
        return NULL;
    return phys_to_virt(m.pa);
}

static void put(struct process *p, uint64_t va, uint8_t seed, unsigned len)
{
    uint8_t *b = ub(p, va);

    for (unsigned i = 0; i < len; i++)
        b[i] = (uint8_t)(seed + i * 3);
}

static int is(struct process *p, uint64_t va, uint8_t seed, unsigned len)
{
    const uint8_t *b = ub(p, va);

    for (unsigned i = 0; i < len; i++)
        if (b[i] != (uint8_t)(seed + i * 3))
            return 0;
    return 1;
}

static int mk(struct process *p, const char *name)
{
    return proc_create(p, name, code, sizeof(code)) == PROC_OK;
}

static void test_basic(void)
{
    struct process *a = &P[0], *b = &P[1];
    uint64_t f0 = pmm_free_count();
    unsigned c0 = channel_count();
    int64_t ha, hb;

    if (!mk(a, "ipc-a") || !mk(b, "ipc-b")) {
        expect(0, "setup: two processes");
        return;
    }
    uint64_t f1 = pmm_free_count();
    expect(channel_create(a, HANDLE_RIGHTS, b, HANDLE_RIGHTS, &ha, &hb) == 0 && ha == 1 && hb == 1 &&
           channel_count() == c0 + 1 && pmm_free_count() == f1 - 2,
           "trusted setup: one channel, handle 1 in each process, one queue frame per direction");

    /* Lengths: 0, 256 and 257; the receive buffer must hold the whole message. */
    put(a, UB, 0x40, 256);
    expect(channel_send(a, ha, UB, 0) == 0 && channel_send(a, ha, UB, 256) == 0 &&
           channel_send(a, ha, UB, 257) == -VOS_EINVAL, "send: 0 and 256 bytes accepted, 257 refused");
    expect(channel_recv(b, hb, UB, 0, 0) == 0, "a zero-length message is received with length 0");
    memset(ub(b, UB), 0, 256);
    expect(channel_recv(b, hb, UB, 255, 0) == -VOS_EMSGSIZE && ub(b, UB)[0] == 0,
           "a 255-byte buffer for a 256-byte message: -VOS_EMSGSIZE, nothing written, message kept");
    expect(channel_recv(b, hb, UB, 4096, 0) == 256 && is(b, UB, 0x40, 256),
           "the same message then arrives whole with a large enough buffer");
    expect(channel_recv(b, hb, UB, 256, 0) == -VOS_EAGAIN, "empty queue, peer open: would block");

    /* FIFO, capacity, and the kernel's order-sensitive record of the stream. */
    int sent = 1;
    for (unsigned i = 0; i < CHANNEL_QUEUE; i++) {
        put(a, UB, (uint8_t)(i * 11), 100 + i);
        sent &= channel_send(a, ha, UB, 100 + i) == 0;
    }
    expect(sent && channel_send(a, ha, UB, 1) == -VOS_EAGAIN, "16 messages queue up; the 17th: -VOS_EAGAIN");
    int fifo = 1;
    for (unsigned i = 0; i < CHANNEL_QUEUE; i++)
        fifo &= channel_recv(b, hb, UB2, 256, 0) == (int64_t)(100 + i) && is(b, UB2, (uint8_t)(i * 11), 100 + i);
    const struct endpoint *eb = handle_endpoint(b, hb);
    expect(fifo && eb && eb->in_msgs == eb->out_msgs && eb->in_hash == eb->out_hash && eb->count == 0,
           "messages arrive in order and intact (in and out streams hash equal)");

    /* Bad user buffers change nothing. */
    expect(channel_send(a, ha, 0x500000, 8) == -VOS_EFAULT && eb->count == 0 &&
           channel_send(a, ha, KERNEL_VMA + 0x100000, 8) == -VOS_EFAULT && eb->count == 0,
           "send from unmapped or kernel memory: -VOS_EFAULT, nothing queued");
    put(a, UB, 0x77, 8);
    channel_send(a, ha, UB, 8);
    expect(channel_recv(b, hb, PROC_CODE_VA, 256, 0) == -VOS_EFAULT && eb->count == 1 &&
           channel_recv(b, hb, KERNEL_VMA + 0x100000, 256, 0) == -VOS_EFAULT && eb->count == 1 &&
           channel_recv(b, hb, UB, 256, 0) == 8 && is(b, UB, 0x77, 8),
           "receive into read-only code or kernel memory: -VOS_EFAULT, the message stays queued");

    /* Handles are per process and validated. */
    expect(channel_send(a, 0, UB, 1) == -VOS_EBADHANDLE && channel_send(a, -1, UB, 1) == -VOS_EBADHANDLE &&
           channel_send(a, 2, UB, 1) == -VOS_EBADHANDLE && channel_send(a, 0x7FFFFFFF, UB, 1) == -VOS_EBADHANDLE &&
           channel_send(a, ha | (5 << 8), UB, 1) == -VOS_EBADHANDLE,
           "0, negative, unissued, huge and wrong-generation handles: -VOS_EBADHANDLE");

    /* Peer closure: drain, then EPIPE; double close; stale values. */
    put(a, UB, 0x11, 3);
    channel_send(a, ha, UB, 3);
    channel_send(a, ha, UB, 2);
    expect(handle_close(a, ha) == 0 && handle_close(a, ha) == -VOS_EBADHANDLE &&
           channel_send(a, ha, UB, 1) == -VOS_EBADHANDLE, "close works once; the closed handle is dead");
    expect(channel_recv(b, hb, UB, 256, 0) == 3 && channel_recv(b, hb, UB, 256, 0) == 2 &&
           channel_recv(b, hb, UB, 256, 0) == -VOS_EPIPE && channel_send(b, hb, UB, 1) == -VOS_EPIPE,
           "after the peer closed: queued messages drain, then -VOS_EPIPE for receive and send");
    expect(handle_close(b, hb) == 0 && channel_count() == c0 && pmm_free_count() == f1,
           "both ends closed: the channel and its two frames are released");

    /* A recycled slot gets a new value; the old one stays invalid. */
    int64_t ha2, hb2;
    expect(channel_create(a, HANDLE_RIGHTS, b, HANDLE_RIGHTS, &ha2, &hb2) == 0 && ha2 != ha && (ha2 & 0xFF) == 1 &&
           channel_send(a, ha, UB, 1) == -VOS_EBADHANDLE && channel_send(a, ha2, UB, 1) == 0,
           "a reused slot has a new generation: the old handle value cannot reach the new channel");

    /* Destroying a process closes its handles; the peer sees EPIPE. */
    proc_destroy(a);
    expect(channel_recv(b, hb2, UB, 256, 0) == 1 && channel_recv(b, hb2, UB, 256, 0) == -VOS_EPIPE,
           "proc_destroy closes the process's handles; the peer drains and gets -VOS_EPIPE");
    proc_destroy(b);
    expect(channel_count() == c0 && pmm_free_count() == f0, "everything released with the processes");
}

static void test_rights_and_ownership(void)
{
    struct process *a = &P[0], *b = &P[1], *c = &P[2];
    uint64_t f0 = pmm_free_count();
    int64_t ha, hb, hb2, hc;

    if (!mk(a, "ipc-a") || !mk(b, "ipc-b") || !mk(c, "ipc-c")) {
        expect(0, "setup: three processes");
        return;
    }
    expect(channel_create(a, HANDLE_SEND, b, HANDLE_RECV, &ha, &hb) == 0 &&
           channel_create(b, HANDLE_RIGHTS, c, HANDLE_RIGHTS, &hb2, &hc) == 0 && hb2 == 2,
           "setup: a send-only and a receive-only end; b has a second handle");
    put(a, UB, 1, 4);
    expect(channel_recv(a, ha, UB, 256, 0) == -VOS_EACCES && channel_send(b, hb, UB, 4) == -VOS_EACCES &&
           channel_send(a, ha, UB, 4) == 0 && channel_recv(b, hb, UB, 256, 0) == 4,
           "rights are enforced: receive on a send-only end and send on a receive-only end refused");
    expect(channel_send(a, hb2, UB, 4) == -VOS_EBADHANDLE && channel_send(c, 2, UB, 4) == -VOS_EBADHANDLE,
           "another process's handle value means nothing in this process's table");
    expect(handle_close(c, hb) == 0 && handle_endpoint(c, hb) == NULL && handle_endpoint(b, hb) != NULL,
           "closing handle 1 in c closes c's own endpoint, never b's handle 1");
    proc_destroy(a);
    proc_destroy(b);
    proc_destroy(c);
    expect(pmm_free_count() == f0 && channel_count() == 0, "rights tests leave nothing behind");
}

static void test_exhaustion(void)
{
    uint64_t f0 = pmm_free_count();
    int64_t h1, h2;
    int ok = 1;

    for (int i = 0; i < 6; i++)
        ok &= mk(&P[i], "ipc-x");
    unsigned made = 0;
    while (channel_create(&P[0], HANDLE_RIGHTS, &P[1], HANDLE_RIGHTS, &h1, &h2) == 0)
        made++;
    expect(ok && made == HANDLE_MAX, "a full handle table: channel_create fails with -VOS_ENOMEM");
    while (channel_create(&P[2], HANDLE_RIGHTS, &P[3], HANDLE_RIGHTS, &h1, &h2) == 0)
        made++;
    uint64_t f1 = pmm_free_count();
    int full = channel_create(&P[4], HANDLE_RIGHTS, &P[5], HANDLE_RIGHTS, &h1, &h2);
    expect(made == CHANNEL_MAX && full == -VOS_ENOMEM && pmm_free_count() == f1 && P[4].handles[0].ep == NULL,
           "16 channels in use: the next one fails with -VOS_ENOMEM and changes nothing");
    for (int i = 0; i < 6; i++)
        proc_destroy(&P[i]);
    expect(channel_count() == 0 && pmm_free_count() == f0, "destroying the processes releases all 16 channels");
}

int selftest_ipc(void)
{
    failures = cases = 0;
    test_basic();
    test_rights_and_ownership();
    test_exhaustion();
    serial_puts("selftest: ipc ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

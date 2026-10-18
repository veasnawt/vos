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

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

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

/* N2.5: channels and handle slots reused many times; every value is new; audits hold throughout. */
static void test_reuse(void)
{
    struct process *a = &P[0], *b = &P[1];
    uint64_t f0 = pmm_free_count();
    int64_t ha, hb, prev_a = 0;
    int ok = mk(a, "ipc-a") && mk(b, "ipc-b"), fresh = 1, audits = 1;
    uint64_t f1 = pmm_free_count();      /* with both processes */

    for (int i = 0; ok && i < 100; i++) {
        ok &= channel_create(a, HANDLE_RIGHTS, b, HANDLE_RIGHTS, &ha, &hb) == 0;
        fresh &= ha != prev_a && channel_send(a, prev_a, UB, 1) == -VOS_EBADHANDLE;
        put(a, UB, (uint8_t)i, 16);
        ok &= channel_send(a, ha, UB, 16) == 0 && channel_recv(b, hb, UB2, 256, 0) == 16 && is(b, UB2, (uint8_t)i, 16);
        if (i % 2)
            ok &= handle_close(a, ha) == 0 && handle_close(b, hb) == 0;
        else
            ok &= handle_close(b, hb) == 0 && handle_close(a, ha) == 0;
        audits &= channel_audit() == NULL && handles_audit(a) == NULL && handles_audit(b) == NULL;
        prev_a = ha;
    }
    expect(ok && fresh && audits && channel_count() == 0 && pmm_free_count() == f1,
           "100 channels created, used and closed (both close orders): every handle value new, audits hold");
    proc_destroy(a);
    proc_destroy(b);
    expect(pmm_free_count() == f0 && channel_count() == 0, "100 reuses leave nothing behind");
}

int selftest_ipc(void)
{
    failures = cases = 0;
    test_basic();
    test_rights_and_ownership();
    test_exhaustion();
    test_reuse();
    expect(channel_audit() == NULL && sched_audit() == NULL && proc_audit() == NULL,
           "channel, scheduler and process audits hold after the IPC tests");
    serial_puts("selftest: ipc ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* ---- N2.4 ELF scenarios (VOS_TEST_CASE=ipc) ---- */

/*
 * Each module whose command line starts with "s-" is a scenario, written in
 * the trusted GRUB configuration (tests/x86_64/ipc.grub.cfg); the module's
 * content is ignored. Keys:
 *   a=MOD b=MOD|none    programs (GRUB module names) for processes a and b
 *   nochan              no channel; otherwise one channel a <-> b, handle 1 in each
 *   ra=, rb=            rights of each end: send | recv | both (default both)
 *   order=              ab | b-blocked (b starts, must block, then a) | a-then-b | b-then-a
 *   kill=a|b            kill that process 3 ticks after the start; kblocked: it must be
 *                       blocked at that moment
 *   ea=, eb=            exit:N | fault:V | killed (by the kernel's "test kill")
 *   ia=N[/M], ib=...    kernel record: N messages arrived for it, M taken out (default M = N),
 *                       and if N == M the in and out streams hash equal (order and content)
 *   repeat=N            run N times; frames, channels, processes and threads back each time
 * The verdict line "ipc: <command line> => PASS|FAIL (...)" is printed by the kernel only.
 */
#include "irq.h"
#include "loader.h"
#include "thread.h"

#define EXP_EXIT    1
#define EXP_FAULT   2
#define EXP_KILLED  3

struct scen {
    const char *a, *b;
    size_t alen, blen;
    int nochan, kill, kblocked, repeat;
    uint32_t ra, rb;
    char order[16];
    int ekind[2];
    int64_t eval[2];
    int has_i[2];
    uint64_t in[2], out[2];
};

static const struct boot_info *sbi;

static int word(const char *s, size_t n, const char *w)
{
    return strlen(w) == n && memcmp(s, w, n) == 0;
}

static uint64_t num(const char *s, size_t n, int *ok)
{
    uint64_t v = 0;

    if (n == 0)
        *ok = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            *ok = 0;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    return v;
}

static uint32_t rights(const char *v, size_t n, int *ok)
{
    if (word(v, n, "send"))
        return HANDLE_SEND;
    if (word(v, n, "recv"))
        return HANDLE_RECV;
    if (word(v, n, "both"))
        return HANDLE_RIGHTS;
    *ok = 0;
    return 0;
}

static void expectation(const char *v, size_t n, int *kind, int64_t *val, int *ok)
{
    if (word(v, n, "killed")) {
        *kind = EXP_KILLED;
    } else if (n > 5 && memcmp(v, "exit:", 5) == 0) {
        *kind = EXP_EXIT;
        *val = (int64_t)num(v + 5, n - 5, ok);
    } else if (n > 6 && memcmp(v, "fault:", 6) == 0) {
        *kind = EXP_FAULT;
        *val = (int64_t)num(v + 6, n - 6, ok);
    } else {
        *ok = 0;
    }
}

static int parse_scen(const char *s, struct scen *c)
{
    int ok = 1;

    memset(c, 0, sizeof(*c));
    c->ra = c->rb = HANDLE_RIGHTS;
    c->repeat = 1;
    memcpy(c->order, "ab", 3);
    while (*s && *s != ' ')
        s++;
    while (*s) {
        while (*s == ' ')
            s++;
        const char *k = s;
        size_t kl = 0, vl = 0;
        while (k[kl] && k[kl] != ' ' && k[kl] != '=')
            kl++;
        const char *v = k + kl + (k[kl] == '=');
        while (k[kl] == '=' && v[vl] && v[vl] != ' ')
            vl++;
        s = v + vl;
        if (!kl)
            continue;
        if (word(k, kl, "a")) {
            c->a = v;
            c->alen = vl;
        } else if (word(k, kl, "b")) {
            c->b = word(v, vl, "none") ? NULL : v;
            c->blen = vl;
        } else if (word(k, kl, "nochan")) {
            c->nochan = 1;
        } else if (word(k, kl, "kblocked")) {
            c->kblocked = 1;
        } else if (word(k, kl, "ra")) {
            c->ra = rights(v, vl, &ok);
        } else if (word(k, kl, "rb")) {
            c->rb = rights(v, vl, &ok);
        } else if (word(k, kl, "order") && vl < sizeof(c->order)) {
            memcpy(c->order, v, vl);
            c->order[vl] = 0;
        } else if (word(k, kl, "kill")) {
            c->kill = word(v, vl, "a") ? 1 : word(v, vl, "b") ? 2 : (ok = 0);
        } else if (word(k, kl, "ea") || word(k, kl, "eb")) {
            int i = k[1] == 'b';
            expectation(v, vl, &c->ekind[i], &c->eval[i], &ok);
        } else if (word(k, kl, "ia") || word(k, kl, "ib")) {
            int i = k[1] == 'b';
            size_t sl = 0;
            while (sl < vl && v[sl] != '/')
                sl++;
            c->has_i[i] = 1;
            c->in[i] = num(v, sl, &ok);
            c->out[i] = sl < vl ? num(v + sl + 1, vl - sl - 1, &ok) : c->in[i];
        } else if (word(k, kl, "repeat")) {
            c->repeat = (int)num(v, vl, &ok);
        } else {
            ok = 0;
        }
    }
    if (!c->a || !c->ekind[0] || (c->b && !c->ekind[1]) || (!c->b && !c->nochan) || c->repeat < 1)
        ok = 0;
    return ok;
}

static const struct boot_module *scen_module(const char *name, size_t n)
{
    for (uint32_t i = 0; i < sbi->module_count; i++)
        if (strlen(sbi->modules[i].cmdline) == n && memcmp(sbi->modules[i].cmdline, name, n) == 0)
            return &sbi->modules[i];
    return NULL;
}

static int load(struct process *p, const char *name, size_t n)
{
    const struct boot_module *m = scen_module(name, n);
    struct elf_image im;
    int eerr;

    return m && load_elf(p, m->cmdline, phys_to_virt(m->start), m->end - m->start, &im, &eerr) == PROC_OK;
}

struct outcome {
    int state, status;
    const char *reason;
    uint64_t vector, ipc_in, ipc_out;
    int hash_equal;
};

static const char *judge(const struct scen *c, int i, const struct outcome *o)
{
    if (c->ekind[i] == EXP_EXIT && (o->state != PROC_EXITED || o->status != c->eval[i]))
        return o->state == PROC_EXITED ? "wrong exit status" : "did not exit";
    if (c->ekind[i] == EXP_FAULT && (o->state != PROC_KILLED || o->vector != (uint64_t)c->eval[i] ||
                                      !o->reason || streq(o->reason, "test kill")))
        return "did not fault as expected";
    if (c->ekind[i] == EXP_KILLED && (o->state != PROC_KILLED || !streq(o->reason, "test kill")))
        return "was not killed by the kernel";
    if (c->has_i[i] && (o->ipc_in != c->in[i] || o->ipc_out != c->out[i] ||
                        (c->in[i] == c->out[i] && !o->hash_equal)))
        return "the kernel's message record differs (count, order or content)";
    return NULL;
}

static const char *run_once(const struct scen *c, struct outcome o[2])
{
    struct process *pa = &P[0], *pb = &P[1];
    struct process *pp[2] = { pa, c->b ? pb : NULL };
    int64_t ha, hb;
    int blocked_ok = 1;

    if (!load(pa, c->a, c->alen))
        return "program a could not be loaded";
    if (c->b && !load(pb, c->b, c->blen)) {
        proc_destroy(pa);
        return "program b could not be loaded";
    }
    if (!c->nochan && channel_create(pa, c->ra, pb, c->rb, &ha, &hb) != 0)
        return "channel setup failed";

    if (word(c->order, strlen(c->order), "b-blocked")) {
        proc_start(pb);
        thread_sleep(3);
        blocked_ok = thread_state(pb->tid) == THREAD_BLOCKED;
        proc_start(pa);
    } else if (word(c->order, strlen(c->order), "a-then-b")) {
        proc_start(pa);
        if (c->ekind[0] != EXP_KILLED)
            proc_wait(pa);
        if (pb->state == PROC_READY)
            proc_start(pb);
    } else if (word(c->order, strlen(c->order), "b-then-a")) {
        proc_start(pb);
        if (c->ekind[1] != EXP_KILLED)
            proc_wait(pb);
        proc_start(pa);
    } else {
        proc_start(pa);
        if (pp[1])
            proc_start(pb);
    }
    if (c->kill) {
        struct process *t = pp[c->kill - 1];
        thread_sleep(3);
        if (c->kblocked && thread_state(t->tid) != THREAD_BLOCKED) {
            blocked_ok = 0;
            serial_puts("ipc:   kill target not blocked: thread state ");
            serial_dec((uint64_t)thread_state(t->tid));
            serial_puts(" proc state ");
            serial_dec((uint64_t)t->state);
            serial_puts("\r\n");
        }
        proc_kill(t, "test kill");
    }
    for (int i = 0; i < 2; i++)                 /* those expected to end on their own */
        if (pp[i] && c->ekind[i] != EXP_KILLED)
            proc_wait(pp[i]);
    for (int i = 0; i < 2; i++) {               /* the rest are ended by the kernel */
        if (!pp[i])
            continue;
        proc_kill(pp[i], "test kill");
        proc_wait(pp[i]);
        o[i].state = pp[i]->state;
        o[i].status = pp[i]->exit_status;
        o[i].reason = pp[i]->reason;
        o[i].vector = pp[i]->trap.vector;
        o[i].ipc_in = pp[i]->ipc_in;
        o[i].ipc_out = pp[i]->ipc_out;
        o[i].hash_equal = pp[i]->ipc_in_hash == pp[i]->ipc_out_hash;
        proc_destroy(pp[i]);
    }
    return blocked_ok ? NULL : "the receiver was not blocked when expected";
}

int selftest_ipc_scenarios(const struct boot_info *bi)
{
    unsigned n = 0, failed = 0;
    uint64_t f0 = pmm_free_count();
    unsigned t0 = thread_count();

    sbi = bi;
    proc_test_tick_budget(400);
    for (uint32_t i = 0; i < bi->module_count; i++) {
        const char *cmd = bi->modules[i].cmdline;
        struct scen c;
        const char *why = NULL;

        if (cmd[0] != 's' || cmd[1] != '-')
            continue;
        n++;
        if (!parse_scen(cmd, &c))
            why = "malformed scenario";
        for (int r = 0; !why && r < c.repeat; r++) {
            struct outcome o[2];

            memset(o, 0, sizeof(o));
            why = run_once(&c, o);
            for (int k = 0; !why && k < 2; k++)
                if (k == 0 || c.b)
                    why = judge(&c, k, &o[k]);
            if (!why && (pmm_free_count() != f0 || channel_count() != 0 || proc_count() != 0 ||
                         thread_count() != t0 || !thread_canaries_intact()))
                why = "resources not back to baseline";
        }
        serial_puts("ipc: ");
        serial_puts(cmd);
        serial_puts(why ? " => FAIL (" : " => PASS");
        if (why) {
            serial_puts(why);
            serial_puts(")");
        }
        serial_puts("\r\n");
        failed += why != NULL;
    }
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("ipc: ");
    serial_dec(n);
    serial_puts(" scenarios, ");
    serial_dec(failed);
    serial_puts(" failed\r\n");
    return (int)failed + (n == 0);
}

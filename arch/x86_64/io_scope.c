#include "io_scope.h"
#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "panic.h"
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
#include "storage_stress.h"
#endif

struct ticket {
    uint64_t id, owner, tid;
    struct io_scope *scope;
    unsigned request, abandoned;
    int (*guard)(void);
};

static struct ticket tickets[IO_SCOPE_TICKETS];
static uint64_t next_id = 1;
_Static_assert(sizeof(tickets) <= 128, "scope ticket budget");

static uint64_t issue(uint64_t *serial)
{
    if (!*serial || *serial == UINT64_MAX)
        return 0;
    return (*serial)++;
}

static struct ticket *lookup(uint64_t id)
{
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++)
        if (id && tickets[i].id == id)
            return &tickets[i];
    return NULL;
}

static struct ticket *current(void)
{
    struct thread *t = thread_current();

    for (unsigned i = 0; t && i < IO_SCOPE_TICKETS; i++)
        if (tickets[i].scope && tickets[i].tid == t->id)
            return &tickets[i];
    return NULL;
}

struct io_scope *io_scope_current(void)
{
    struct ticket *t = current();
    return t ? t->scope : NULL;
}

int io_scope_begin(struct io_scope *s, uint64_t owner)
{
    struct ticket *free = NULL;
    struct thread *thread = thread_current();
    uint64_t now = irq_ticks();

    if (!s || !owner || !thread || irq_in_handler() || (read_rflags() & RFLAGS_IF) ||
        now > UINT64_MAX - IO_SCOPE_TICK_LIMIT)
        return IO_SCOPE_INVALID;
    if (current())
        return IO_SCOPE_BUSY;
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++) {
        if (tickets[i].id && tickets[i].owner == owner)
            return IO_SCOPE_BUSY;
        if (!tickets[i].id)
            free = &tickets[i];
    }
    if (!free || next_id == UINT64_MAX)  /* no wrap, no stale ticket reuse */
        return IO_SCOPE_BUSY;
    memset(s, 0, sizeof(*s));
    s->ticket = issue(&next_id);
    s->deadline = now + IO_SCOPE_TICK_LIMIT;
    s->logical_left = IO_SCOPE_LOGICAL_LIMIT;
    s->reads_left = IO_SCOPE_READ_LIMIT;
    s->waits_left = IO_SCOPE_WAIT_LIMIT;
    *free = (struct ticket){ .id = s->ticket, .owner = owner, .tid = thread->id, .scope = s };
    return IO_SCOPE_OK;
}

int io_scope_end(struct io_scope *s)
{
    struct ticket *t = current();

    if (!t || t->scope != s || t->id != s->ticket || (t->request && !t->abandoned))
        panic("io_scope: end with wrong owner or live request");
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
    storage_stress_event(STRESS_SCOPE_END, t->owner);
#endif
    t->scope = NULL;
    t->tid = 0;
    t->guard = NULL;
    if (!t->request)
        memset(t, 0, sizeof(*t));
    s->ticket = 0;
    return s->result;
}

void io_scope_stop(int reason)
{
    struct io_scope *s = io_scope_current();

    if (s && (!s->result || reason == IO_SCOPE_INTERRUPTED))
        s->result = reason;
}

int io_scope_check(void)
{
    struct ticket *t = current();
    struct io_scope *s = t ? t->scope : NULL;

    if (s && irq_ticks() >= s->deadline)
        io_scope_stop(IO_SCOPE_TIMEOUT);
    if (s && !s->result && t->guard) {
        int reason = t->guard();
        if (reason)
            io_scope_stop(reason);
    }
    return s ? s->result : IO_SCOPE_OK;
}

void io_scope_set_guard(int (*guard)(void))
{
    struct ticket *t = current();
    if (!t || t->guard || !guard)
        panic("io_scope: invalid binding guard");
    t->guard = guard;
}

static int consume(unsigned *remaining)
{
    int r = io_scope_check();
    if (r)
        return r;
    if (!*remaining) {
        io_scope_stop(IO_SCOPE_BUDGET);
        return IO_SCOPE_BUDGET;
    }
    --*remaining;
    return IO_SCOPE_OK;
}

int io_scope_logical(void)
{
    struct io_scope *s = io_scope_current();
    return s ? consume(&s->logical_left) : IO_SCOPE_OK;
}

int io_scope_read(void)
{
    struct io_scope *s = io_scope_current();
    return s ? consume(&s->reads_left) : IO_SCOPE_OK;
}

uint64_t io_scope_deadline(uint64_t local)
{
    struct io_scope *s = io_scope_current();
    return s && (!local || s->deadline < local) ? s->deadline : local;
}

int io_scope_wait(struct wait_queue *q, uint64_t local)
{
    struct io_scope *s = io_scope_current();

    if (s && consume(&s->waits_left))
        return THREAD_INTERRUPTED;
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
    storage_stress_event(STRESS_WAIT, (uint64_t)q);
#endif
    int r = thread_block_until(q, io_scope_deadline(local));
    if (s && r == THREAD_INTERRUPTED)
        io_scope_stop(IO_SCOPE_INTERRUPTED);
    if (s && io_scope_check())
        return THREAD_INTERRUPTED;       /* caller unwinds; scope retains the exact reason */
    return r;
}

void io_scope_cancel(uint64_t owner)
{
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++)
        if (tickets[i].id && tickets[i].owner == owner && tickets[i].scope)
            tickets[i].scope->result = IO_SCOPE_INTERRUPTED;
}

int io_scope_request_begin(uint64_t *id)
{
    struct ticket *t = current();
    *id = 0;
    if (io_scope_check())
        return IO_SCOPE_INTERRUPTED;
    if (t) {
        if (t->request)
            return IO_SCOPE_BUSY;
        t->request = 1;
        *id = t->id;
    }
    return IO_SCOPE_OK;
}

void io_scope_request_abandon(uint64_t id)
{
    struct ticket *t = lookup(id);
    if (!id)
        return;
    if (!t || !t->request || t->abandoned)
        panic("io_scope: invalid abandonment");
    t->abandoned = 1;
}

void io_scope_request_end(uint64_t id)
{
    struct ticket *t = lookup(id);
    if (!id)
        return;
    if (!t || !t->request)
        panic("io_scope: double request retirement");
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
    storage_stress_event(STRESS_RETIRE, id);
#endif
    t->request = t->abandoned = 0;
    if (!t->scope)
        memset(t, 0, sizeof(*t));
}

int io_scope_request_valid(uint64_t id, uint64_t tid, int abandoned)
{
    struct ticket *t = lookup(id);
    return !id || (t && t->request && t->abandoned == (unsigned)abandoned &&
                   (abandoned || (t->scope && t->tid == tid)));
}

unsigned io_scope_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++)
        n += tickets[i].id != 0;
    return n;
}

const char *io_scope_audit(void)
{
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++) {
        const struct ticket *t = &tickets[i];
        if (!t->id) {
            if (t->owner || t->scope || t->tid || t->request || t->abandoned || t->guard)
                return "free storage ticket retains state";
            continue;
        }
        if (!t->owner || t->request > 1 || t->abandoned > t->request)
            return "invalid storage ticket";
        for (unsigned j = 0; j < i; j++)
            if (tickets[j].id && (tickets[j].id == t->id || tickets[j].owner == t->owner))
                return "duplicate storage ticket identity";
        if (t->scope) {
            if (!t->tid || thread_state(t->tid) == THREAD_FREE || thread_state(t->tid) == THREAD_DEAD ||
                t->scope->ticket != t->id)
                return "storage scope outlived its thread";
        } else if (t->tid || t->guard || !t->request || !t->abandoned) {
            return "detached ticket is not an abandoned request";
        }
    }
    return NULL;
}

void io_scope_thread_exit(void)
{
    if (current())
        panic("io_scope: thread exited without unwinding");
}

#ifdef VOS_TEST
uint64_t io_scope_test_issue(uint64_t *serial)
{
    return issue(serial);
}

void io_scope_test_limits(struct io_scope *s, unsigned logical, unsigned reads,
                          unsigned waits, uint64_t deadline)
{
    if (s != io_scope_current() || logical > IO_SCOPE_LOGICAL_LIMIT || reads > IO_SCOPE_READ_LIMIT ||
        waits > IO_SCOPE_WAIT_LIMIT)
        panic("io_scope: invalid test limit");
    s->logical_left = logical;
    s->reads_left = reads;
    s->waits_left = waits;
    s->deadline = deadline;
}
#endif

#ifdef VOS_TEST
#include "storage_observe.h"

void io_scope_test_observe(struct observe_ticket out[IO_SCOPE_TICKETS])
{
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++) {
        const struct ticket *t = &tickets[i];
        out[i].id = t->id;
        out[i].pid = t->owner;
        out[i].tid = t->tid;
        out[i].scope = (uint64_t)t->scope;
        out[i].guard = (uint64_t)t->guard;
        out[i].request = t->request;
        out[i].abandoned = t->abandoned;
        if (t->scope) {
            out[i].deadline = t->scope->deadline;
            out[i].logical = t->scope->logical_left;
            out[i].reads = t->scope->reads_left;
            out[i].waits = t->scope->waits_left;
            out[i].result = t->scope->result;
        }
    }
}
#endif

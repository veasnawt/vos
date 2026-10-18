#include "channel.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "process.h"
#include "syscall.h"
#include "usercopy.h"

static struct channel channels[CHANNEL_MAX];
static uint64_t next_channel_id = 1;

unsigned channel_count(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < CHANNEL_MAX; i++)
        n += channels[i].used;
    return n;
}

static uint8_t *slot_bytes(const struct endpoint *ep, unsigned i)
{
    return (uint8_t *)phys_to_virt(ep->queue_pa) + (uint64_t)i * CHANNEL_MSG_MAX;
}

/* FNV-1a over (length, bytes): an order-sensitive record of a message stream. */
static uint64_t hash_msg(uint64_t h, const uint8_t *b, unsigned len)
{
    h = (h ^ len) * 0x100000001B3ULL;
    for (unsigned i = 0; i < len; i++)
        h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}

/* ---- handles ---- */

static int64_t handle_value(unsigned slot, uint16_t gen)
{
    return ((int64_t)gen << 8) | (int64_t)(slot + 1);
}

/* p's slot for handle h, or -1: only p's own table is ever consulted. */
static int handle_slot(const struct process *p, int64_t h)
{
    if (h <= 0 || h > 0xFFFFFF)
        return -1;
    unsigned slot = (unsigned)(h & 0xFF) - 1;
    uint16_t gen = (uint16_t)(h >> 8);

    if (slot >= HANDLE_MAX || !p->handles[slot].ep || p->handles[slot].gen != gen)
        return -1;
    if (p->handles[slot].ep->owner != p || !p->handles[slot].ep->open)
        panic("channel: handle table entry points to an endpoint the process does not own");
    return (int)slot;
}

const struct endpoint *handle_endpoint(const struct process *p, int64_t h)
{
    int s = handle_slot(p, h);

    return s < 0 ? NULL : p->handles[s].ep;
}

static int free_slot(const struct process *p)
{
    for (unsigned i = 0; i < HANDLE_MAX; i++)
        if (!p->handles[i].ep)
            return (int)i;
    return -1;
}

static int64_t install(struct process *p, unsigned slot, struct endpoint *ep, uint32_t rights)
{
    p->handles[slot].ep = ep;
    p->handles[slot].rights = rights;
    ep->owner = p;
    ep->open = 1;
    return handle_value(slot, p->handles[slot].gen);
}

/* ---- channels ---- */

static void release_channel(struct channel *c)
{
    for (int i = 0; i < 2; i++) {
        if (c->ep[i].open || c->ep[i].waiters.head)
            panic("channel: releasing a channel with an open endpoint or a waiter");
        if (pmm_free_frame(c->ep[i].queue_pa) != PMM_OK)
            panic("channel: queue frame release failed");
    }
    memset(c, 0, sizeof(*c));
}

int channel_create(struct process *a, uint32_t rights_a, struct process *b, uint32_t rights_b,
                   int64_t *ha, int64_t *hb)
{
    struct channel *c = NULL;
    uint64_t f0, f1;
    int sa, sb;

    *ha = *hb = 0;
    if (!a || !b || (rights_a & ~HANDLE_RIGHTS) || (rights_b & ~HANDLE_RIGHTS))
        return -VOS_EINVAL;
    for (unsigned i = 0; i < CHANNEL_MAX && !c; i++)
        if (!channels[i].used)
            c = &channels[i];
    sa = free_slot(a);
    sb = a == b ? -1 : free_slot(b);
    if (a == b) {                       /* both ends in one process: two different slots */
        for (unsigned i = (unsigned)sa + 1; sa >= 0 && i < HANDLE_MAX && sb < 0; i++)
            if (!a->handles[i].ep)
                sb = (int)i;
    }
    if (!c || sa < 0 || sb < 0)
        return -VOS_ENOMEM;
    if (pmm_alloc_frame(&f0) != PMM_OK)
        return -VOS_ENOMEM;
    if (pmm_alloc_frame(&f1) != PMM_OK) {
        pmm_free_frame(f0);
        return -VOS_ENOMEM;
    }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->id = next_channel_id++;
    c->ep[0].ch = c->ep[1].ch = c;
    c->ep[0].queue_pa = f0;
    c->ep[1].queue_pa = f1;
    *ha = install(a, (unsigned)sa, &c->ep[0], rights_a);
    *hb = install(b, (unsigned)sb, &c->ep[1], rights_b);
    return 0;
}

static struct endpoint *peer_of(struct endpoint *ep)
{
    return ep == &ep->ch->ep[0] ? &ep->ch->ep[1] : &ep->ch->ep[0];
}

/* Close one endpoint: drop its queue, wake a receiver blocked on the peer, free the channel when both are closed. */
static void endpoint_close(struct endpoint *ep)
{
    struct endpoint *peer = peer_of(ep);

    if (ep->waiters.head)
        panic("channel: closing an endpoint its owner is blocked on");
    if (ep->owner) {                    /* the kernel's record of this endpoint's traffic */
        ep->owner->ipc_in += ep->in_msgs;
        ep->owner->ipc_out += ep->out_msgs;
        ep->owner->ipc_in_hash ^= ep->in_hash;
        ep->owner->ipc_out_hash ^= ep->out_hash;
    }
    ep->open = 0;
    ep->owner = NULL;
    ep->count = ep->head = 0;           /* queued messages can never be received now */
    if (peer->open)
        thread_wake_all(&peer->waiters);
    else
        release_channel(ep->ch);
}

int64_t handle_close(struct process *p, int64_t h)
{
    int s = handle_slot(p, h);

    if (s < 0)
        return -VOS_EBADHANDLE;
    struct endpoint *ep = p->handles[s].ep;
    p->handles[s].ep = NULL;
    p->handles[s].rights = 0;
    p->handles[s].gen++;                /* the old value never names this slot again */
    endpoint_close(ep);
    return 0;
}

void handles_close_all(struct process *p)
{
    for (unsigned i = 0; i < HANDLE_MAX; i++)
        if (p->handles[i].ep)
            handle_close(p, handle_value(i, p->handles[i].gen));
}

int64_t channel_send(struct process *p, int64_t h, uint64_t ubuf, uint64_t len)
{
    int s = handle_slot(p, h);

    if (s < 0)
        return -VOS_EBADHANDLE;
    if (!(p->handles[s].rights & HANDLE_SEND))
        return -VOS_EACCES;
    if (len > CHANNEL_MSG_MAX)
        return -VOS_EINVAL;
    struct endpoint *peer = peer_of(p->handles[s].ep);
    if (!peer->open)
        return -VOS_EPIPE;
    if (peer->count == CHANNEL_QUEUE)
        return -VOS_EAGAIN;

    unsigned tail = (peer->head + peer->count) % CHANNEL_QUEUE;
    uint8_t *dst = slot_bytes(peer, tail);
    if (copy_from_user(p->root, dst, ubuf, len) != 0)
        return -VOS_EFAULT;             /* nothing committed */
    peer->len[tail] = (uint16_t)len;
    peer->count++;
    peer->in_msgs++;
    peer->in_hash = hash_msg(peer->in_hash, dst, (unsigned)len);
    thread_wake_one(&peer->waiters);
    return 0;
}

int64_t channel_recv(struct process *p, int64_t h, uint64_t ubuf, uint64_t cap, int may_block)
{
    for (;;) {
        int s = handle_slot(p, h);

        if (s < 0)
            return -VOS_EBADHANDLE;
        if (!(p->handles[s].rights & HANDLE_RECV))
            return -VOS_EACCES;
        struct endpoint *ep = p->handles[s].ep;

        if (ep->count) {
            unsigned len = ep->len[ep->head];
            uint8_t *src = slot_bytes(ep, ep->head);

            if (len > cap)
                return -VOS_EMSGSIZE;   /* stays queued: never truncated */
            if (copy_to_user(p->root, ubuf, src, len) != 0)
                return -VOS_EFAULT;     /* stays queued */
            ep->out_msgs++;
            ep->out_hash = hash_msg(ep->out_hash, src, len);
            ep->head = (ep->head + 1) % CHANNEL_QUEUE;
            ep->count--;
            return len;
        }
        if (!peer_of(ep)->open)
            return -VOS_EPIPE;
        if (!may_block)
            return -VOS_EAGAIN;
        if (p->kill_reason)
            return CHANNEL_KILLED;
        /* Empty and the peer is open: wait. Nothing can run between the check and the block. */
        thread_block(&ep->waiters);
        if (p->kill_reason)
            return CHANNEL_KILLED;
    }
}

/* ---- consistency audit (N2.5) ---- */

static const char *endpoint_audit(const struct endpoint *ep)
{
    unsigned n = 0;

    if (ep->count > CHANNEL_QUEUE || ep->head >= CHANNEL_QUEUE)
        return "endpoint queue indices out of range";
    if (pmm_frame_state_in(pmm_kernel(), ep->queue_pa) != PMM_FRAME_ALLOCATED)
        return "endpoint queue frame is not allocated";
    if (!ep->open) {
        if (ep->owner || ep->count || ep->waiters.head)
            return "a closed endpoint has an owner, messages or waiters";
        return NULL;
    }
    int held = 0;
    for (unsigned i = 0; ep->owner && i < HANDLE_MAX; i++)
        held |= ep->owner->handles[i].ep == ep;
    if (!held)
        return "an open endpoint is not in its owner's handle table";
    for (const struct thread *w = ep->waiters.head; w; w = w->next) {
        if (++n > 1)
            return "more than one thread waits on an endpoint";
        if (w->state != THREAD_BLOCKED || w->waiting_on != &ep->waiters || w->proc != ep->owner)
            return "an endpoint waiter is not its owner's blocked thread";
    }
    return NULL;
}

const char *channel_audit(void)
{
    for (unsigned i = 0; i < CHANNEL_MAX; i++) {
        const struct channel *c = &channels[i];

        if (!c->used) {
            if (c->ep[0].waiters.head || c->ep[1].waiters.head || c->ep[0].open || c->ep[1].open)
                return "a free channel record has endpoints or waiters";
            continue;
        }
        if (!c->ep[0].open && !c->ep[1].open)
            return "a channel with both ends closed was not released";
        for (int k = 0; k < 2; k++) {
            if (c->ep[k].ch != c)
                return "an endpoint points to the wrong channel";
            const char *why = endpoint_audit(&c->ep[k]);
            if (why)
                return why;
        }
    }
    return NULL;
}

const char *handles_audit(const struct process *p)
{
    for (unsigned i = 0; i < HANDLE_MAX; i++) {
        const struct endpoint *ep = p->handles[i].ep;

        if (!ep)
            continue;
        if (!ep->ch || !ep->ch->used || !ep->open || ep->owner != p || (p->handles[i].rights & ~HANDLE_RIGHTS))
            return "a handle names an endpoint its process does not own";
    }
    return NULL;
}

/*
 * Channels and handles (N2.4, ABI v1).
 *
 * A channel connects two endpoints. Each endpoint belongs to exactly one
 * process (its owner) through one entry of that process's handle table, and
 * has its own receive queue: the messages the peer sent to it, oldest
 * first, at most CHANNEL_QUEUE of at most CHANNEL_MSG_MAX bytes. The queue's
 * bytes live in one frame from the physical allocator (CHANNEL_QUEUE x 256
 * bytes = 4 KiB), owned by the channel and freed with it; the records are a
 * static pool. Nothing is shared with user space: send copies from the
 * sender's memory into the queue, receive copies from the queue into the
 * receiver's memory (copy_from_user / copy_to_user, validated first).
 *
 * Handles. A process's handle table has HANDLE_MAX slots; a handle value is
 * (generation << 8) | (slot + 1), so 0 and every value not issued to this
 * process, or issued but closed since (the generation advances), are
 * invalid (-VOS_EBADHANDLE). A handle is only ever looked up in the calling
 * process's own table, so another process's handle values mean nothing.
 * Rights (HANDLE_SEND, HANDLE_RECV) are fixed when the trusted kernel code
 * creates the channel and checked on every operation (-VOS_EACCES).
 *
 * Lifetime. Closing a handle closes its endpoint: messages still queued to
 * it are dropped (nobody can receive them any more), a receiver blocked on
 * the peer endpoint is woken (it drains its queue, then gets -VOS_EPIPE),
 * and once both endpoints are closed the channel record and both frames
 * are released. A process closes all its handles when it ends (exit, fault
 * or kill) and in proc_destroy. Blocked receivers sit on their own
 * endpoint's wait queue; an endpoint is never closed while its owner is
 * blocked on it (the owner is the only thread that can close it, and it is
 * not blocked then; a kill first wakes the thread).
 *
 * Synchronisation: one CPU, the kernel runs with IF = 0 and is never
 * preempted, so every operation, including "queue empty, so block", is
 * atomic with respect to every other thread: a wakeup cannot be lost.
 */
#ifndef VOS_X86_64_CHANNEL_H
#define VOS_X86_64_CHANNEL_H

#include <stdint.h>

#include "thread.h"

#define CHANNEL_MAX         16
#define CHANNEL_QUEUE       16      /* messages per direction */
#define CHANNEL_MSG_MAX     256
#define HANDLE_MAX          8       /* per process */

#define HANDLE_SEND         0x1u
#define HANDLE_RECV         0x2u
#define HANDLE_RIGHTS       (HANDLE_SEND | HANDLE_RECV)

struct process;
struct channel;

struct endpoint {
    struct channel *ch;
    struct process *owner;              /* NULL once closed */
    int open;
    uint64_t queue_pa;                  /* CHANNEL_QUEUE slots of CHANNEL_MSG_MAX bytes */
    uint16_t len[CHANNEL_QUEUE];
    unsigned head, count;
    struct wait_queue waiters;          /* the owner, blocked in receive */
    /* Evidence for tests: messages and an order-sensitive hash, in and out. */
    uint64_t in_msgs, out_msgs, in_hash, out_hash;
};

struct channel {
    int used;
    uint64_t id;                        /* never reused */
    struct endpoint ep[2];
};

/* One entry of a process's handle table. */
struct handle {
    struct endpoint *ep;                /* NULL: free slot */
    uint32_t rights;
    uint16_t gen;
};

/*
 * Trusted setup (kernel code only): a new channel between a and b with the
 * given rights; the new handle values go to *ha and *hb. Returns 0 or
 * -VOS_ENOMEM (no record, no frame, or no free handle slot in a or b; then
 * nothing changed) or -VOS_EINVAL.
 */
int channel_create(struct process *a, uint32_t rights_a, struct process *b, uint32_t rights_b,
                   int64_t *ha, int64_t *hb);

/* The operations of ABI v1, for process p (the caller), with user addresses in p's space. */
int64_t channel_send(struct process *p, int64_t h, uint64_t ubuf, uint64_t len);
/* may_block 0: return -VOS_EAGAIN instead of blocking (kernel tests). */
int64_t channel_recv(struct process *p, int64_t h, uint64_t ubuf, uint64_t cap, int may_block);
int64_t handle_close(struct process *p, int64_t h);
/* Close every handle p still holds (process end and teardown). */
void handles_close_all(struct process *p);

/* Channels in use (records not free). */
unsigned channel_count(void);
/* The endpoint behind p's handle h, or NULL (tests and diagnostics). */
const struct endpoint *handle_endpoint(const struct process *p, int64_t h);

/*
 * N2.5 consistency audits: every used channel has an open end, queues within
 * bounds in allocated frames, open ends in their owner's handle table, at
 * most one waiter, which is the owner's blocked thread; closed ends and free
 * records hold nothing. handles_audit checks one process's table. NULL if
 * all hold, else the first broken invariant.
 */
const char *channel_audit(void);
const char *handles_audit(const struct process *p);

/* Returned by channel_recv when the caller was killed while blocked (never reaches user space). */
#define CHANNEL_KILLED      (-1000)

#endif

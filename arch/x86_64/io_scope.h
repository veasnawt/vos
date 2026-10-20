/* Optional bounded storage calls (N3.5 I1). Kernel-only; no application ABI.
 * One CPU, IF=0, thread context. The caller owns the scope until end();
 * a detached request keeps only a scalar ticket, never a caller pointer.
 * Unscoped storage clients retain their existing behavior.
 */
#ifndef VOS_X86_64_IO_SCOPE_H
#define VOS_X86_64_IO_SCOPE_H

#include <stdint.h>
#include "thread.h"

#define IO_SCOPE_TICK_LIMIT     200
#define IO_SCOPE_LOGICAL_LIMIT  512
#define IO_SCOPE_READ_LIMIT     256
#define IO_SCOPE_WAIT_LIMIT     1024
#define IO_SCOPE_TICKETS        2

#define IO_SCOPE_OK             0
#define IO_SCOPE_BUSY           1
#define IO_SCOPE_INTERRUPTED    2
#define IO_SCOPE_TIMEOUT        3
#define IO_SCOPE_BUDGET         4
#define IO_SCOPE_INVALID        5

struct io_scope {
    uint64_t ticket, deadline;
    unsigned logical_left, reads_left, waits_left;
    int result;
};

/* owner is a trusted nonzero, non-reused PID (test kernel callers use distinct keys).
 * No nesting; at most one active/debt ticket per owner. No allocation or wait.
 */
int io_scope_begin(struct io_scope *scope, uint64_t owner);
int io_scope_end(struct io_scope *scope);
struct io_scope *io_scope_current(void);
int io_scope_check(void);
int io_scope_logical(void);             /* ext2 device-access step, including cache hits */
int io_scope_read(void);                /* block backend submission, including fallback */
void io_scope_stop(int reason);         /* sticky first outcome; interruption wins */
uint64_t io_scope_deadline(uint64_t local);
int io_scope_wait(struct wait_queue *q, uint64_t local_deadline);
void io_scope_cancel(uint64_t owner);   /* remember kill before a waiter can requeue */

/* Driver's exactly-once request reference. id=0 means an unscoped request. */
int io_scope_request_begin(uint64_t *id);
void io_scope_request_abandon(uint64_t id);
void io_scope_request_end(uint64_t id);
int io_scope_request_valid(uint64_t id, uint64_t tid, int abandoned);
unsigned io_scope_count(void);
const char *io_scope_audit(void);
void io_scope_thread_exit(void);        /* assert no scope/request still belongs to current thread */

#ifdef VOS_TEST
uint64_t io_scope_test_issue(uint64_t *serial);
void io_scope_test_limits(struct io_scope *scope, unsigned logical, unsigned reads,
                          unsigned waits, uint64_t deadline);
#endif
#endif

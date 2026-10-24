/* Gate A: scalar event capture only. Scheduling barriers belong to Gate B.
 * IF=0 thread context; no allocation, I/O, waiting or hidden retirement.
 */
#ifndef VOS_STORAGE_STRESS_H
#define VOS_STORAGE_STRESS_H
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
#include "bootinfo.h"
#include "io_scope.h"
enum stress_event_kind { STRESS_SCOPE_END, STRESS_WAIT, STRESS_POLL,
                         STRESS_COMPLETE, STRESS_RETIRE, STRESS_PROGRESS, STRESS_EVENT_KINDS };
struct stress_event {
    uint64_t sequence, ticket, tid, identity;
    unsigned kind, logical, reads, waits;
    int result;
};
#if defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d)
#define STRESS_EVENTS 128
#else
#define STRESS_EVENTS 256
#endif
struct stress_trace {
    unsigned count, overflow;
    uint64_t kinds[STRESS_EVENT_KINDS];
    struct stress_event events[STRESS_EVENTS];
};
struct process;
struct exc_frame;
#ifdef VOS_TEST_CASE_storage_stress_b
int selftest_storage_stress_b(const struct boot_info *bi);
void storage_stress_b_progress(struct process *, const struct exc_frame *, int64_t);
#endif
extern struct stress_trace storage_stress_trace;
void storage_stress_event(unsigned kind, uint64_t identity);
int selftest_storage_stress(const struct boot_info *bi);
/* Gate C: after every existing declaration, so no line moves in the Gate A/B images; the progress hook is a
 * complete statement appended to process.c's shared progress line. */
#ifdef VOS_TEST_CASE_storage_stress_c
int selftest_storage_stress_c(const struct boot_info *bi);
void storage_stress_c_progress(struct process *, const struct exc_frame *, int64_t);
#define STORAGE_STRESS_C_PROGRESS(p, f, res) storage_stress_c_progress(p, f, res);
#else
#define STORAGE_STRESS_C_PROGRESS(p, f, res)
#endif
/* Gate D: after every existing declaration, so no line moves in the Gate A/B/C images. Its hooks are complete
 * statements trailing existing lines (storage_stress.c, kernel.c) or one directive line (process.c). */
#ifdef VOS_TEST_CASE_storage_stress_d
int selftest_storage_stress_d(const struct boot_info *bi);
void storage_stress_d_progress(struct process *, const struct exc_frame *, int64_t);
void storage_stress_d_event(unsigned kind, uint64_t identity);
#define STORAGE_STRESS_D_EVENT(kind, identity) storage_stress_d_event(kind, identity);
#else
#define STORAGE_STRESS_D_EVENT(kind, identity)
#endif
#endif
#endif

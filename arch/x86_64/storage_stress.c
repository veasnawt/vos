#include "storage_stress.h"
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
struct stress_trace storage_stress_trace;

void storage_stress_event(unsigned kind, uint64_t identity)
{
    struct stress_trace *t = &storage_stress_trace; STORAGE_STRESS_D_EVENT(kind, identity)
    struct thread *me = thread_current();
    struct io_scope *scope = io_scope_current();
    if (kind >= STRESS_EVENT_KINDS || t->count == STRESS_EVENTS) {
        t->overflow = 1;
        return;
    }
    t->kinds[kind]++;
    struct stress_event *event = &t->events[t->count++];
    *event = (struct stress_event){
        .sequence = t->count, .kind = kind, .identity = identity,
        .tid = me ? me->id : 0, .ticket = scope ? scope->ticket : 0,
        .logical = scope ? scope->logical_left : 0,
        .reads = scope ? scope->reads_left : 0,
        .waits = scope ? scope->waits_left : 0,
        .result = scope ? scope->result : 0,
    };
}
#endif

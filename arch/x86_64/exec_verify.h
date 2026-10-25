#ifndef VOS_X86_64_EXEC_VERIFY_H
#define VOS_X86_64_EXEC_VERIFY_H
#include <stdint.h>
struct process;
struct boot_info;
struct resv_table;
struct exc_frame;
enum exec_event {
 EXEC_EV_QUEUED=1, EXEC_EV_BEFORE_LOAD, EXEC_EV_AFTER_LOAD, EXEC_EV_BEFORE_START,
 EXEC_EV_AFTER_START, EXEC_EV_REAP, EXEC_EV_BEFORE_PUBLISH, EXEC_EV_AFTER_PUBLISH,
 EXEC_EV_CLEANUP, EXEC_EV_AFTER_HANDOFF
};
#if defined(VOS_EXEC_VERIFY) || defined(VOS_EXEC_HOST)
void exec_verify_event(enum exec_event, struct process *, struct process *);
void exec_verify_source(const uint8_t **, uint64_t *);
#endif
#ifdef VOS_EXEC_VERIFY
#define SYS_EXEC_VERIFY 0x7e64
int exec_verify_requested(const char *);
int exec_verify_prepare(const struct boot_info *, const struct resv_table *);
int exec_verify_run(void);
int64_t exec_verify_syscall(struct process *, const struct exc_frame *);
#endif
#endif

#ifndef VOS_HANDOFF_VERIFY_H
#define VOS_HANDOFF_VERIFY_H
#include <stdint.h>
struct boot_info;struct process;struct exc_frame;
enum exec_event;
#ifdef VOS_HANDOFF_VERIFY
#define SYS_HANDOFF_VERIFY 0x7e65
int handoff_verify_requested(const char *);
int handoff_verify_active(void);
int handoff_verify_run(const struct boot_info *);
void handoff_verify_event(enum exec_event,struct process *,struct process *);
void handoff_verify_source(const uint8_t **,uint64_t *);
int64_t handoff_verify_syscall(struct process *,const struct exc_frame *);
#endif
#endif

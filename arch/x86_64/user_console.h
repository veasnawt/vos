/* ABI v3 console: one explicitly granted process, no inherited authority. */
#ifndef VOS_X86_64_USER_CONSOLE_H
#define VOS_X86_64_USER_CONSOLE_H
#include <stdint.h>
struct process;
struct exc_frame;
int user_console_grant(struct process *p);
void user_console_revoke(struct process *p);
int64_t user_console_syscall(struct process *p, const struct exc_frame *frame);
#endif

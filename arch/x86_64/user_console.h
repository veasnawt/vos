/* Explicit ABI v3/v4 console ownership; kernel-only foreground leases. */
#ifndef VOS_X86_64_USER_CONSOLE_H
#define VOS_X86_64_USER_CONSOLE_H
#include <stdint.h>
struct process;
struct exc_frame;
int user_console_grant(struct process *);
void user_console_revoke(struct process *);
int64_t user_console_syscall(struct process *,const struct exc_frame *);
int user_console_owned(const struct process *);
int user_console_reader_active(const struct process *);
int user_console_debug_visible(const struct process *);
int user_console_begin_exec(struct process *,struct process *);
int user_console_finish_exec(struct process *,struct process *,int restore_allowed);
int user_console_exec_active(const struct process *,const struct process *);
int user_console_lease_active(void);
struct console_lease_stats {uint64_t transferred,restored,abandoned,revoked;};
const struct console_lease_stats *user_console_lease_stats(void);
#ifdef VOS_CONSOLE_HOST
uint64_t console_host_root(void);
uint8_t console_host_in(uint16_t);
void console_host_out(uint16_t,uint8_t);
#endif
#endif

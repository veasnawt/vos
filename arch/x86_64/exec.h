#ifndef VOS_X86_64_EXEC_H
#define VOS_X86_64_EXEC_H
#include <stdint.h>
#include "app_catalog.h"
#include "../../user/include/vos/apps.h"
struct process;
struct exc_frame;
#define EXEC_ENUM 1u
#define EXEC_RUN 2u
#define EXEC_RIGHTS (EXEC_ENUM | EXEC_RUN)
#define EXEC_FOREGROUND 4u
#define EXEC_ALL (EXEC_RIGHTS | EXEC_FOREGROUND)
struct exec_stats {
    uint64_t submitted, loaded, started, reclaimed, completed, cancelled, failed;
};
int exec_service_init(const struct app_catalog *);
int exec_service_stop(void);
int exec_grant_install(struct process *, uint32_t rights, const uint64_t *ids, unsigned count);
void exec_grant_revoke(struct process *);
void exec_cancel_request(struct process *);
void exec_process_cancel(struct process *);
void exec_process_end(struct process *);
int64_t exec_syscall(struct process *, const struct exc_frame *);
const struct exec_stats *exec_statistics(void);
int exec_request_busy(void);
#if defined(VOS_EXEC_HOST) || defined(VOS_EXEC_VERIFY)
/* Verification-only refresh; production provisioning remains READY-only. */
int exec_test_regrant(struct process *,uint32_t,const uint64_t *,unsigned);
#endif
#ifdef VOS_EXEC_HOST
uint64_t exec_host_root(void);
void exec_host_pump(void);
#endif
#endif

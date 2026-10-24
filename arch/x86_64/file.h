/* N3.5 read-only nodes. One CPU, IF=0, thread context.
 * Native pointer arguments are trusted kernel memory, never user pointers.
 * file_syscall uses a live kernel trap frame after syscall.c's ABI v2 gate;
 * user addresses are decoded only at validation/copy boundaries.
 * READY processes allow trusted setup/tests; RUNNING processes require their own thread.
 */
#ifndef VOS_X86_64_FILE_H
#define VOS_X86_64_FILE_H
#include "ext2.h"
#include "process.h"

#define FILE_MAX 8
#define FILE_INITIAL_MAX 2
#define FILE_READ 1u
#define FILE_STAT 2u
#define FILE_ENUMERATE 4u
#define FILE_TRAVERSE 8u
#define FILE_RIGHTS UINT64_C(15) /* full-width input validation, before narrowing */
#define FILE_TOKEN_DOMAIN (1ULL << 62)
#define FILE_SERIAL_MAX (FILE_TOKEN_DOMAIN - 1)
struct exc_frame;
int64_t file_syscall(struct process *p, const struct exc_frame *frame);
/* Kernel-native error aliases match the additive ABI v2 errors. */
#define FILE_ENOENT 10
#define FILE_ENOTDIR 11
#define FILE_EISDIR 12
#define FILE_ENAMETOOLONG 13
#define FILE_ELOOP 14
#define FILE_EIO 15
#define FILE_EUNAVAIL 16
#define FILE_ESTALE 17
#define FILE_ETIMEDOUT 18
#define FILE_EINTR 19
#define FILE_EUNSUPP 20

struct file_grant {
    unsigned index;
    uint64_t rights;        /* validate before storing the node's compact rights */
    const char *path;       /* trusted relative path from volume root, never app input */
    uint64_t length;
};
/* Explicit trusted configuration; no automatic production grants.
 * Detach refuses live nodes, active work or outstanding I1 scope debt.
 */
int file_volume_attach(struct process *setup_owner, blk_id_t device);
int file_volume_detach(void);
int file_grants_install(struct process *p, const struct file_grant *grants, unsigned count);
int64_t file_initial(struct process *p, uint64_t index);
int64_t file_open_at(struct process *p, uint64_t base, const char *path, uint64_t length, uint64_t rights);
int64_t file_close(struct process *p, uint64_t token);
/* Native kernel results, not wire records. Error leaves output unchanged.
 * read <=4096 uses private staging; zero ignores output/volume after rights.
 * next validates cookies, skips dots, checks target inode and commits once.
 */
int file_stat(struct process *p, uint64_t token, struct ext2_stat *out);
int64_t file_read_at(struct process *p, uint64_t token, void *out, uint64_t length, uint64_t offset);
int file_next(struct process *p, uint64_t token, uint64_t *cookie, struct ext2_dirent *out);
void file_close_all(struct process *p);   /* cancel/defer release until active work unwinds */
int file_process_busy(const struct process *p); /* destruction/start forbidden while busy */
unsigned file_handle_count(void);
unsigned file_session_count(void);
const char *file_audit(void);
#ifdef VOS_TEST
uint64_t file_test_issue(uint64_t *serial);
void file_test_last_tokens(void);       /* irreversible monotonic jump in test image */
#endif
/* N3.6 Gate C publication hold, after an operation's private result and before its final cancellation
 * check: appended to the line that completes open/stat/directory operations in file.c, and in read_at
 * included at statement position (file_hold.inc), so no line moves in any other image. */
#if defined(VOS_TEST) && defined(VOS_TEST_CASE_storage_stress_c)
void storage_stress_c_hold(int op_result);
#define FILE_PUBLICATION_HOLD(r) storage_stress_c_hold(r);
#else
#define FILE_PUBLICATION_HOLD(r)
#endif
#endif

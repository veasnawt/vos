#include "syscall.h"
#include "exception.h"
#include "process.h"

int64_t syscall_dispatch(struct process *p, struct exc_frame *f)
{
    (void)p;
    switch (f->rax) {
    default:
        return -VOS_ENOSYS;
    }
}

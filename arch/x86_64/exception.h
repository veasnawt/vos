/* CPU exception dispatch. */
#ifndef VOS_X86_64_EXCEPTION_H
#define VOS_X86_64_EXCEPTION_H

#include <stdint.h>

/* Register state saved by the stubs in entry.S; field order is the push order. */
struct exc_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;             /* error is 0 when the CPU pushes none */
    uint64_t rip, cs, rflags, rsp, ss;  /* pushed by the CPU */
};

void exception_handler(struct exc_frame *f);
/* Human-readable name of an exception vector, e.g. "#GP general protection fault". */
const char *exception_name(uint64_t vector);

#endif

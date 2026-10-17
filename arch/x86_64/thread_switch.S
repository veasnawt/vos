/*
 * Kernel-thread context switch (N2.2).
 *
 * void context_switch(uint64_t *save_rsp, uint64_t load_rsp)
 *   Called like any function, with IF = 0. Pushes the callee-saved registers
 *   (rbp, rbx, r12-r15) on the current stack, stores RSP in *save_rsp, loads
 *   load_rsp and pops the same registers from the other stack, then returns
 *   on it. Caller-saved registers, flags (IF stays 0, DF is 0 by the ABI) and
 *   CR3 are not part of the context: the compiler already treats the call as
 *   clobbering them, every thread shares the kernel address space, and the
 *   kernel has no FPU/SIMD state.
 *
 * idle_halt
 *   The idle thread's wait: "sti; hlt", then cli. STI delays interrupt
 *   recognition by one instruction, so an interrupt pending at the sti, or
 *   arriving while halted, is taken with the return address idle_after_hlt;
 *   the only kernel code that ever runs with IF = 1 is this hlt and the cli
 *   at idle_after_hlt (sched_idle_irq accepts a ring-0 IRQ only there).
 *
 * thread_start
 *   The return address of a new thread's initial frame (thread.c). Entered by
 *   context_switch's ret with RSP 16-byte aligned; it calls thread_first_run,
 *   which finishes the switch, runs the entry function and exits the thread.
 */
.section .text
.code64

.global context_switch
.type context_switch, @function
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size context_switch, . - context_switch

.global idle_halt, idle_after_hlt
.type idle_halt, @function
idle_halt:
    sti
    hlt
idle_after_hlt:
    cli
    ret
.size idle_halt, . - idle_halt

.global thread_start
.type thread_start, @function
thread_start:
    xor %ebp, %ebp                  /* outermost frame */
    call thread_first_run           /* does not return */
    ud2
.size thread_start, . - thread_start

.section .note.GNU-stack, "", @progbits

/*
 * User-mode test programs and a test helper (VOS_TEST only).
 *
 * The programs are position-independent byte sequences in .rodata. The tests
 * copy them into a fresh user code page; they are never executed here.
 * Programs that need an argument read it from the 8-byte slot ending the
 * program, which the test fills in the copied page.
 */
.set SYS_EXIT, 0
.set ENTRY_RSP, 0x00007FFFFFFFEFF0

.macro PROG name
.global up_\name, up_\name\()_end
.endm

.section .rodata
.code64

/* exit(42) */
PROG exit42
up_exit42:
    mov $SYS_EXIT, %eax
    mov $42, %edi
    int $0x80
    ud2
up_exit42_end:

/*
 * Check the entry state, then exit with 0x5A if all is as specified, or with
 * the number of the first failed check: 1 a register not zero, 3 RFLAGS not
 * 0x2, 4 RSP not the specified value, 5 RSP not 16-byte aligned, 6 the 16
 * bytes at RSP not zero.
 */
PROG regcheck
up_regcheck:
    pushfq                          /* entry RFLAGS, before any instruction changes it */
    or %rbx, %rax
    or %rcx, %rax
    or %rdx, %rax
    or %rsi, %rax
    or %rdi, %rax
    or %rbp, %rax
    or %r8, %rax
    or %r9, %rax
    or %r10, %rax
    or %r11, %rax
    or %r12, %rax
    or %r13, %rax
    or %r14, %rax
    or %r15, %rax
    mov $1, %edi
    test %rax, %rax
    jnz 9f
    pop %rcx
    mov $3, %edi
    cmp $0x2, %rcx
    jne 9f
    mov $4, %edi
    movabs $ENTRY_RSP, %rdx
    cmp %rdx, %rsp
    jne 9f
    mov $5, %edi
    test $15, %spl
    jnz 9f
    mov $6, %edi
    cmpq $0, (%rsp)
    jne 9f
    cmpq $0, 8(%rsp)
    jne 9f
    mov $0x5A, %edi
9:  mov $SYS_EXIT, %eax
    int $0x80
    ud2
up_regcheck_end:

/* An unsupported system-call number. */
PROG badsys
up_badsys:
    mov $99, %eax
    int $0x80
    ud2
up_badsys_end:

/* Enter the kernel with DF set: the kernel must still work (it clears DF). */
PROG std_exit
up_std_exit:
    std
    mov $SYS_EXIT, %eax
    mov $7, %edi
    int $0x80
    ud2
up_std_exit_end:

/*
 * uint64_t call_check_callee_saved(int (*fn)(void *), void *arg, int *result)
 * Loads known values into rbx, rbp, r12-r15, calls fn(arg), stores its result
 * and returns the OR of (register ^ expected): 0 if fn preserved them all.
 */
.section .text
.global call_check_callee_saved
.type call_check_callee_saved, @function
call_check_callee_saved:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rdx                      /* result pointer; 7 pushes keep RSP 16-aligned for the call */
    mov %rdi, %rax
    mov %rsi, %rdi
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rbp
    movabs $0x3333333333333333, %r12
    movabs $0x4444444444444444, %r13
    movabs $0x5555555555555555, %r14
    movabs $0x6666666666666666, %r15
    call *%rax
    popq %rdx
    mov %eax, (%rdx)
    movabs $0x1111111111111111, %rax
    xor %rax, %rbx
    movabs $0x2222222222222222, %rax
    xor %rax, %rbp
    movabs $0x3333333333333333, %rax
    xor %rax, %r12
    movabs $0x4444444444444444, %rax
    xor %rax, %r13
    movabs $0x5555555555555555, %rax
    xor %rax, %r14
    movabs $0x6666666666666666, %rax
    xor %rax, %r15
    mov %rbx, %rax
    or %rbp, %rax
    or %r12, %rax
    or %r13, %rax
    or %r14, %rax
    or %r15, %rax
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size call_check_callee_saved, . - call_check_callee_saved

.section .note.GNU-stack, "", @progbits

/*
 * User-mode test programs and a test helper (VOS_TEST only).
 *
 * The programs are position-independent byte sequences in .rodata. The tests
 * copy them into a fresh user code page; they are never executed here.
 * Programs that need an argument read it from the 8-byte slot ending the
 * program, which the test fills in the copied page.
 */
.set SYS_EXIT, 0
.set SYS_DEBUG_WRITE, 1
.set VOS_EINVAL, 2
.set VOS_EFAULT, 3
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

/*
 * Load known values into every register except rax and rsp, set CF and DF,
 * leave canaries in the red zone, make an unknown system call and check that
 * only rax changed (to -VOS_ENOSYS = -1). Exit status: 0 = all preserved,
 * 2 wrong result, 3 flags lost, 4 a register changed, 5 red zone touched,
 * 6 RSP changed, 7 a second (huge) number did not return -1.
 */
.macro CHECKREG reg, val
    movabs $\val, %rax
    cmp %rax, \reg
    jne 8f
.endm
PROG enosys
up_enosys:
    movq $0x5A5A5A5A, %rax
    mov %rax, -16(%rsp)
    mov %rax, -64(%rsp)
    mov %rax, -128(%rsp)
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rcx
    movabs $0x3333333333333333, %rdx
    movabs $0x4444444444444444, %rsi
    movabs $0x5555555555555555, %rdi
    movabs $0x6666666666666666, %rbp
    movabs $0x7777777777777777, %r8
    movabs $0x8888888888888888, %r9
    movabs $0x9999999999999999, %r10
    movabs $0xAAAAAAAAAAAAAAAA, %r11
    movabs $0xBBBBBBBBBBBBBBBB, %r12
    movabs $0xCCCCCCCCCCCCCCCC, %r13
    movabs $0xDDDDDDDDDDDDDDDD, %r14
    movabs $0xEEEEEEEEEEEEEEEE, %r15
    mov $1234, %eax
    stc
    std
    int $0x80
    pushfq                          /* the flags as returned, before any instruction changes them */
    cmp $-1, %rax
    jne 2f
    popq %rax
    test $1, %al                    /* CF */
    jz 3f
    test $0x400, %eax               /* DF */
    jz 3f
    cld
    CHECKREG %rbx, 0x1111111111111111
    CHECKREG %rcx, 0x2222222222222222
    CHECKREG %rdx, 0x3333333333333333
    CHECKREG %rsi, 0x4444444444444444
    CHECKREG %rdi, 0x5555555555555555
    CHECKREG %rbp, 0x6666666666666666
    CHECKREG %r8, 0x7777777777777777
    CHECKREG %r9, 0x8888888888888888
    CHECKREG %r10, 0x9999999999999999
    CHECKREG %r11, 0xAAAAAAAAAAAAAAAA
    CHECKREG %r12, 0xBBBBBBBBBBBBBBBB
    CHECKREG %r13, 0xCCCCCCCCCCCCCCCC
    CHECKREG %r14, 0xDDDDDDDDDDDDDDDD
    CHECKREG %r15, 0xEEEEEEEEEEEEEEEE
    cmpq $0x5A5A5A5A, -16(%rsp)
    jne 5f
    cmpq $0x5A5A5A5A, -64(%rsp)
    jne 5f
    cmpq $0x5A5A5A5A, -128(%rsp)
    jne 5f
    movabs $ENTRY_RSP, %rax
    cmp %rax, %rsp
    jne 6f
    mov $-1, %rax                   /* the largest unsigned number */
    int $0x80
    cmp $-1, %rax
    jne 7f
    xor %edi, %edi
    jmp 9f
2:  mov $2, %edi
    jmp 9f
3:  mov $3, %edi
    jmp 9f
8:  mov $4, %edi
    jmp 9f
5:  mov $5, %edi
    jmp 9f
6:  mov $6, %edi
    jmp 9f
7:  mov $7, %edi
9:  mov $SYS_EXIT, %eax
    int $0x80
    ud2
up_enosys_end:

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
 * debug_write twice; after each call check the result, that rdi/rsi still
 * hold the arguments and that every other register is unchanged.
 * Exit 0, or 2 wrong result, 4 a register changed.
 */
PROG hello
up_hello:
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rcx
    movabs $0x3333333333333333, %rdx
    movabs $0x6666666666666666, %rbp
    movabs $0x7777777777777777, %r8
    movabs $0x8888888888888888, %r9
    movabs $0x9999999999999999, %r10
    movabs $0xAAAAAAAAAAAAAAAA, %r11
    movabs $0xBBBBBBBBBBBBBBBB, %r12
    movabs $0xCCCCCCCCCCCCCCCC, %r13
    movabs $0xDDDDDDDDDDDDDDDD, %r14
    movabs $0xEEEEEEEEEEEEEEEE, %r15
    lea 10f(%rip), %rdi
    mov $(11f - 10f), %esi
    mov $SYS_DEBUG_WRITE, %eax
    int $0x80
    cmp $(11f - 10f), %rax
    jne 2f
    lea 10f(%rip), %rax
    cmp %rax, %rdi
    jne 8f
    cmp $(11f - 10f), %rsi
    jne 8f
    CHECKREG %rbx, 0x1111111111111111
    CHECKREG %rcx, 0x2222222222222222
    CHECKREG %rdx, 0x3333333333333333
    CHECKREG %rbp, 0x6666666666666666
    CHECKREG %r8, 0x7777777777777777
    CHECKREG %r9, 0x8888888888888888
    CHECKREG %r10, 0x9999999999999999
    CHECKREG %r11, 0xAAAAAAAAAAAAAAAA
    CHECKREG %r12, 0xBBBBBBBBBBBBBBBB
    CHECKREG %r13, 0xCCCCCCCCCCCCCCCC
    CHECKREG %r14, 0xDDDDDDDDDDDDDDDD
    CHECKREG %r15, 0xEEEEEEEEEEEEEEEE
    lea 11f(%rip), %rdi             /* more instructions, then a second call */
    mov $(12f - 11f), %esi
    mov $SYS_DEBUG_WRITE, %eax
    int $0x80
    cmp $(12f - 11f), %rax
    jne 2f
    CHECKREG %r15, 0xEEEEEEEEEEEEEEEE
    xor %edi, %edi
    jmp 9f
2:  mov $2, %edi
    jmp 9f
8:  mov $4, %edi
9:  mov $SYS_EXIT, %eax
    int $0x80
    ud2
10: .ascii "Hello from VOS!\n"
11: .ascii "second call resumed\n"
12:
up_hello_end:

/*
 * Invalid debug_write arguments through the real system-call path; exit 0 if
 * every call returned the expected value, else the number of the first
 * failed case. The process must stay alive throughout.
 */
.macro DW n, ptr, len, want
    movabs $\ptr, %rdi
    movabs $\len, %rsi
    mov $SYS_DEBUG_WRITE, %eax
    int $0x80
    mov $\n, %edi
    cmp $\want, %rax
    jne 9f
.endm
PROG badptr
up_badptr:
    DW 1, 0, 5, -VOS_EFAULT                         /* null */
    DW 2, 0xFFFFFFFF80100000, 5, -VOS_EFAULT        /* kernel address */
    DW 3, 0x500000, 5, -VOS_EFAULT                  /* unmapped */
    DW 4, 0x400FFE, 5, -VOS_EFAULT                  /* runs off the code page */
    DW 5, 0xFFFFFFFFFFFFFF80, 0x100, -VOS_EFAULT    /* wraps past 2^64 */
    DW 6, 0x0000800000000000, 5, -VOS_EFAULT        /* non-canonical */
    DW 7, 0x00007FFFFFFFFFF0, 0x20, -VOS_EFAULT     /* runs into the non-canonical hole */
    DW 8, 0x00007FFFFFFFA000, 5, -VOS_EFAULT        /* stack guard page */
    DW 9, 0x400000, 257, -VOS_EINVAL                /* longer than 256 */
    DW 10, 0x400000, 0x8000000000000000, -VOS_EINVAL
    DW 11, 0, 0, 0                                  /* zero length: pointer ignored */
    movabs $0x00007FFFFFFFBFFE, %rax                /* "AB" | "CD" across two stack pages */
    movw $0x4241, (%rax)
    movw $0x4443, 2(%rax)
    DW 12, 0x00007FFFFFFFBFFE, 4, 4
    xor %edi, %edi
9:  mov $SYS_EXIT, %eax
    int $0x80
    ud2
up_badptr_end:

/* Attempts to forge kernel output; exit 0 if the whole buffer was accepted. */
PROG forge
up_forge:
    lea 10f(%rip), %rdi
    mov $(11f - 10f), %esi
    mov $SYS_DEBUG_WRITE, %eax
    int $0x80
    mov $2, %edi
    cmp $(11f - 10f), %rax
    jne 9f
    xor %edi, %edi
9:  mov $SYS_EXIT, %eax
    int $0x80
    ud2
10: .ascii "VOS-FORGED-MARKER\n"
    .ascii "VOS-TEST: PASS\r\n"
    .ascii "boot: checks passed\n"
    .byte 0x1B
    .ascii "[2J"
    .byte 0xFF, 0x00, 0x07
11:
up_forge_end:

/* 1000 zero-length writes and 100 unknown calls; exit 0 if all returned as specified. */
PROG many
up_many:
    mov $1000, %ebx
1:  mov $SYS_DEBUG_WRITE, %eax
    xor %edi, %edi
    xor %esi, %esi
    int $0x80
    mov $2, %edi
    test %rax, %rax
    jnz 9f
    dec %ebx
    jnz 1b
    mov $100, %ebx
2:  mov $77, %eax
    int $0x80
    mov $3, %edi
    cmp $-1, %rax
    jne 9f
    dec %ebx
    jnz 2b
    xor %edi, %edi
9:  mov $SYS_EXIT, %eax
    int $0x80
    ud2
up_many_end:

/* ---- faulting programs: each exits with 1 if the expected fault does not happen ---- */
.macro NOFAULT
    mov $SYS_EXIT, %eax
    mov $1, %edi
    int $0x80
    ud2
.endm

PROG ud2
up_ud2:
    ud2
up_ud2_end:

PROG cli                            /* privileged at CPL 3 with IOPL 0: #GP(0) */
up_cli:
    cli
    NOFAULT
up_cli_end:

PROG hlt
up_hlt:
    hlt
    NOFAULT
up_hlt_end:

PROG movcr3
up_movcr3:
    mov %cr3, %rax
    NOFAULT
up_movcr3_end:

PROG outb                           /* port I/O: IOPL 0 and no I/O bitmap */
up_outb:
    mov $0x3F8, %dx
    mov $0x58, %al
    out %al, %dx
    NOFAULT
up_outb_end:

PROG int14                          /* DPL 0 exception gate: #GP(14 * 8 + 2) */
up_int14:
    int $14
    NOFAULT
up_int14_end:

PROG int81                          /* absent gate: #GP(0x81 * 8 + 2) */
up_int81:
    int $0x81
    NOFAULT
up_int81_end:

PROG sgdt                           /* #GP(0) only when CR4.UMIP is set */
up_sgdt:
    sgdt -16(%rsp)
    NOFAULT
up_sgdt_end:

PROG kwrite                         /* write to the kernel address in the slot */
up_kwrite:
    mov 1f(%rip), %rax
    movq $0, (%rax)
    NOFAULT
1:  .quad 0
up_kwrite_end:

PROG kread                          /* read the kernel address in the slot */
up_kread:
    mov 1f(%rip), %rax
    mov (%rax), %rbx
    NOFAULT
1:  .quad 0
up_kread_end:

PROG nullread
up_nullread:
    xor %eax, %eax
    mov (%rax), %rbx
    NOFAULT
up_nullread_end:

PROG codewrite                      /* the code page is read + execute */
up_codewrite:
    lea up_codewrite(%rip), %rax
    movb $0x90, (%rax)
    NOFAULT
up_codewrite_end:

PROG execstack                      /* the stack is NX */
up_execstack:
    jmp *%rsp
up_execstack_end:

PROG guard                          /* push below the stack: into the guard page */
up_guard:
    movabs $0x00007FFFFFFFB000, %rsp
    pushq %rax
    NOFAULT
up_guard_end:

PROG x87                            /* CR0.EM = 1: #NM */
up_x87:
    fld1
    NOFAULT
up_x87_end:

PROG mmx                            /* CR0.EM = 1: #UD (QEMU TCG lets EMMS through, see plan) */
up_mmx:
    pxor %mm0, %mm0
    NOFAULT
up_mmx_end:

PROG sse                            /* CR0.EM = 1 (and CR4.OSFXSR = 0): #UD */
up_sse:
    xorps %xmm0, %xmm0
    NOFAULT
up_sse_end:

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

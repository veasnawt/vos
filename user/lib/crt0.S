/*
 * Process entry for VOS ABI v0 executables.
 *
 * The kernel enters _start in ring 3 with RSP = 0x7FFFFFFFEFF0 (16-byte
 * aligned, the 16 bytes above it zero), every general-purpose register zero,
 * RFLAGS = 0x2 (DF clear) and no floating point (x87 #NM, MMX/SSE #UD).
 *
 * VOS ABI v0 provides no argc/argv/envp, no auxiliary vector, no TLS, no
 * atexit handlers, no stdio and no heap: main takes no arguments, and its
 * return value becomes the exit status.
 */
.section .text.start, "ax"
.code64
.global _start
.type _start, @function
_start:
    mov %rsp, __vos_entry_rsp(%rip) /* kept for later argument passing */
    xor %ebp, %ebp                  /* deepest frame marker (psABI) */
    and $-16, %rsp                  /* already aligned; defensive */
    call main                       /* 16-aligned before the call: (rsp + 8) % 16 == 0 in main */
    mov %eax, %edi
    call vos_exit                   /* does not return */
    ud2
.size _start, . - _start

/*
 * The runtime's only writable state. It also guarantees that the RW PT_LOAD
 * segment is never empty: ld emits an empty segment of size 0 for a program
 * without writable data, which the VOS profile rejects.
 */
.section .data
.balign 8
.global __vos_entry_rsp
.type __vos_entry_rsp, @object
__vos_entry_rsp:
    .quad 0
.size __vos_entry_rsp, 8

.section .note.GNU-stack, "", @progbits

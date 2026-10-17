/* Multiboot v1 entry stub. */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss, "aw", @nobits
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    cld
    mov $stack_top, %esp
    sub $8, %esp         /* keep %esp 16-byte aligned at the call */
    push %ebx            /* multiboot info pointer */
    push %eax            /* multiboot magic */
    call kernel_main
1:  hlt
    jmp 1b
.size _start, . - _start

.section .note.GNU-stack, "", @progbits

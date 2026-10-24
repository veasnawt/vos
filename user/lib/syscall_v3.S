#include <vos/console.h>
.section .text
.macro console_stub name, number
.global \name
.type \name, @function
\name:
    mov $\number, %eax
    int $0x80
    ret
.size \name, . - \name
.endm
console_stub vos_console_read, VOS_SYS_CONSOLE_READ
console_stub vos_console_write, VOS_SYS_CONSOLE_WRITE
console_stub vos_console_clear, VOS_SYS_CONSOLE_CLEAR
.section .note.GNU-stack, "", @progbits

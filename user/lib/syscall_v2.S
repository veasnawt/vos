#include <vos/file.h>
.section .text
.code64
.macro file_stub name, number, fourth=0
.global \name
.type \name, @function
\name:
.if \fourth
    mov %rcx, %r10
.endif
    mov $\number, %eax
    int $0x80
    ret
.size \name, . - \name
.endm
file_stub vos_file_open_at, VOS_SYS_FILE_OPEN_AT, 1
file_stub vos_file_read_at, VOS_SYS_FILE_READ_AT, 1
file_stub vos_file_stat, VOS_SYS_FILE_STAT
file_stub vos_directory_next, VOS_SYS_DIRECTORY_NEXT, 1
file_stub vos_file_initial, VOS_SYS_FILE_INITIAL
.section .note.GNU-stack, "", @progbits

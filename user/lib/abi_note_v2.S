.section .note.vos, "a", @note
.balign 4
    .long 4
    .long 4
    .long 1
    .ascii "VOS\0"
    .long 2
.section .note.GNU-stack, "", @progbits

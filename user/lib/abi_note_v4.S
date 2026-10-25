.section .note.vos, "a", @note
.balign 4
    .long 4, 4, 1
    .ascii "VOS\0"
    .long 4
.section .note.GNU-stack, "", @progbits

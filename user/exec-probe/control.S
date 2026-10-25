/* Verification fixture only; not linked into normal SDK applications. */
.text
.global vos_exec_verify
.type vos_exec_verify,@function
vos_exec_verify:
    mov $0x7e64,%eax
    int $0x80
    ret
.size vos_exec_verify,.-vos_exec_verify
.section .note.GNU-stack,"",@progbits

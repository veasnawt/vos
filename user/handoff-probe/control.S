# Verification-only query; no application hardware access.
.section .text
.global vos_handoff_verify
.type vos_handoff_verify,@function
vos_handoff_verify:
    mov $0x7e65,%eax
    int $0x80
    ret
.size vos_handoff_verify,.-vos_handoff_verify
.section .note.GNU-stack,"",@progbits

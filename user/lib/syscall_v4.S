.text
.global vos_app_next
.type vos_app_next, @function
vos_app_next:
    mov $14, %eax
    int $0x80
    ret
.size vos_app_next, .-vos_app_next
.global vos_app_run_foreground
.type vos_app_run_foreground, @function
vos_app_run_foreground:
    mov $15, %eax
    int $0x80
    ret
.size vos_app_run_foreground, .-vos_app_run_foreground
.section .note.GNU-stack,"",@progbits

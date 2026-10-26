/* ABI v5 uses the existing int80 contract; C/Rust System V arguments. */
.section .text
.macro gui_call name,nr
.global \name
.type \name,@function
\name:
    mov $\nr,%eax
    int $0x80
    ret
.size \name,.-\name
.endm
gui_call vos_gui_create,16
gui_call vos_gui_upload,17
gui_call vos_gui_present,18
gui_call vos_gui_release,19
gui_call vos_gui_event,20
gui_call vos_gui_pointer_state,21
.section .note.GNU-stack,"",@progbits

/*
 * The VOS ABI note for ABI v1 executables (N2.4): identical to abi_note.S
 * except the version. ABI v1 = v0 plus yield, channel_send, channel_recv and
 * handle_close (docs/abi/syscall-v1.md); a v0 executable gets -VOS_ENOSYS
 * for those. Linked instead of abi_note.o, never together with it.
 */
.section .note.vos, "a", @note
.balign 4
    .long 4                         /* n_namesz: "VOS\0" */
    .long 4                         /* n_descsz */
    .long 1                         /* n_type: VOS_NOTE_ABI */
    .ascii "VOS\0"
    .long 1                         /* ABI version 1 */

.section .note.GNU-stack, "", @progbits

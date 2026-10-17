/*
 * The VOS ABI note (plan section 8.4), required exactly once per executable
 * and found by the loader through PT_NOTE, never through section headers.
 * Layout: three 32-bit words, then the name and descriptor, each padded to
 * 4 bytes:
 *   n_namesz = 4, n_descsz = 4, n_type = 1 (VOS_NOTE_ABI),
 *   name = "VOS\0", desc = u32 ABI version (0).
 */
.section .note.vos, "a", @note
.balign 4
    .long 4                         /* n_namesz: "VOS\0" */
    .long 4                         /* n_descsz */
    .long 1                         /* n_type: VOS_NOTE_ABI */
    .ascii "VOS\0"
    .long 0                         /* ABI version 0 */

.section .note.GNU-stack, "", @progbits

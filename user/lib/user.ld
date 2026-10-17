/*
 * VOS ABI v0 executables: static, non-PIE ET_EXEC, x86_64 (plan section 8.4).
 *
 * Exactly three page-aligned PT_LOAD segments in the user image region
 * starting at 0x400000, each in its own pages, never writable and executable:
 *   text    R-X  code (crt0 first)
 *   rodata  R--  constants and the VOS ABI note
 *   data    RW-  initialized data, then .bss
 * plus PT_NOTE (the ABI note, 4-byte aligned) and a non-executable
 * PT_GNU_STACK. Headers are not loaded. No interpreter, dynamic section,
 * TLS, RELRO or GNU property note is produced.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD FLAGS(5);       /* R-X */
    rodata  PT_LOAD FLAGS(4);       /* R-- */
    data    PT_LOAD FLAGS(6);       /* RW- */
    note    PT_NOTE FLAGS(4);
    stack   PT_GNU_STACK FLAGS(6);  /* RW-: the stack is never executable */
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text.start) *(.text .text.*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .note.vos : { KEEP(*(.note.vos)) } :rodata :note

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /*
     * Only metadata VOS does not use is dropped. Anything that would make the
     * program dynamic or need TLS (.interp, .dynamic, .tdata, ...) is NOT
     * discarded here, so it reaches the output and the validator rejects it.
     */
    /DISCARD/ : {
        *(.note.gnu.property) *(.note.gnu.build-id) *(.comment) *(.eh_frame) *(.eh_frame_hdr)
    }
}

/*
 * VOS x86_64 kernel layout.
 *
 * The boot area (.boot.*) runs at its physical address (VMA == LMA) before
 * paging is enabled. The rest of the kernel is linked in the higher half at
 * KERNEL_VMA + LMA and loaded directly after the boot area. Explicit program
 * headers keep code read-only and data non-executable (no RWX segment).
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_LMA = 0x100000;
KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    boot_text PT_LOAD FLAGS(5);     /* R-X */
    boot_data PT_LOAD FLAGS(6);     /* RW- */
    text      PT_LOAD FLAGS(5);     /* R-X */
    rodata    PT_LOAD FLAGS(4);     /* R-- */
    data      PT_LOAD FLAGS(6);     /* RW- */
}

SECTIONS
{
    . = KERNEL_LMA;
    __kernel_phys_start = .;

    .boot.text :
    {
        KEEP(*(.multiboot2))
        *(.boot.text)
        *(.boot.rodata)
    } :boot_text

    .boot.data ALIGN(4K) : { *(.boot.data) } :boot_data

    .boot.bss ALIGN(4K) :
    {
        __boot_bss_start = .;
        *(.boot.bss)
        . = ALIGN(8);
        __boot_bss_end = .;
    } :boot_data

    . = ALIGN(4K) + KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) { *(.text .text.*) } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) { *(.rodata .rodata.*) } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) { *(.data .data.*) } :data

    .bss ALIGN(16) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(8);
        __bss_end = .;
    } :data

    . = ALIGN(4K);
    __kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note .note.*) }
}

/*
 * VOS x86_64 kernel layout.
 *
 * The boot area (.boot.*) runs at its physical address (VMA == LMA) before
 * paging is enabled. The rest of the kernel is linked in the higher half at
 * KERNEL_VMA + LMA and loaded directly after the boot area. Explicit program
 * headers keep code read-only and data non-executable (no RWX segment).
 *
 * The __*_start/__*_end symbols mark the page-aligned permission ranges that
 * kmap.c maps with 4 KiB pages (N2.0 kernel W^X). .text is padded with int3
 * to a page boundary so that no other data (such as the Multiboot2 info
 * block, which GRUB may place in a segment's unused tail) can share an
 * executable page.
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
    __boot_text_end = .;

    .boot.data ALIGN(4K) : { __boot_data_start = .; *(.boot.data) } :boot_data

    .boot.bss ALIGN(4K) :
    {
        __boot_bss_start = .;
        *(.boot.bss)
        . = ALIGN(8);
        __boot_bss_end = .;
    } :boot_data

    . = ALIGN(4K) + KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text =0xCCCCCCCC

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
    } :rodata
    __rodata_end = ALIGN(4K);

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) { __data_start = .; *(.data .data.*) } :data

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

    /* kmap.c covers the image with one 4 KiB page table: the first 2 MiB. */
    ASSERT(__kernel_phys_end <= 0x200000, "kernel image exceeds the 2 MiB covered by kmap.c")
    ASSERT(__boot_bss_end <= __text_start - KERNEL_VMA, "boot area overlaps the kernel text")

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note .note.*) }
}

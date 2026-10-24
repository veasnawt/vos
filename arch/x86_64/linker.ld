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
 *
 * Every kernel page belongs to exactly one PT_LOAD (N3.6): each segment's
 * memory ends on a page boundary, so the segments tile the reservation
 * [__kernel_phys_start, __kernel_phys_end) and a loader, which honours only
 * [p_paddr, p_paddr + p_memsz), can place nothing in a kernel page. Padding
 * is int3 in the boot text, zero in .rodata and NOBITS after .boot.bss and
 * .bss (whose zeroing still ends at __boot_bss_end and __bss_end).
 * scripts/check-kernel-layout.py verifies the program headers.
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
        FILL(0xCCCCCCCC);
        . = ALIGN(4K);
    } :boot_text
    __boot_text_end = .;

    .boot.data ALIGN(4K) : { __boot_data_start = .; *(.boot.data) } :boot_data

    .boot.bss ALIGN(4K) :
    {
        __boot_bss_start = .;
        *(.boot.bss)
        . = ALIGN(8);
        __boot_bss_end = .;
        . = ALIGN(4K);
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
        . = ALIGN(4K);
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
        . = ALIGN(4K);
    } :data

    . = ALIGN(4K);
    __kernel_phys_end = . - KERNEL_VMA;

    /* kmap.c covers the image with two 4 KiB page tables: the first 4 MiB (KMAP_SMALL_SPAN). */
    ASSERT(__kernel_phys_end <= 0x400000, "kernel image exceeds the 4 MiB covered by kmap.c")
    ASSERT(__boot_bss_end <= __text_start - KERNEL_VMA, "boot area overlaps the kernel text")
    /* No page tail outside every PT_LOAD (see above). */
    ASSERT(ADDR(.boot.text) + SIZEOF(.boot.text) == ADDR(.boot.data), "boot text leaves a page tail")
    ASSERT(ADDR(.boot.bss) + SIZEOF(.boot.bss) == __text_start - KERNEL_VMA, "boot data leaves a page tail")
    ASSERT(__text_end == ADDR(.rodata), "text leaves a page tail")
    ASSERT(ADDR(.rodata) + SIZEOF(.rodata) == ADDR(.data), "rodata leaves a page tail")
    ASSERT(ADDR(.bss) + SIZEOF(.bss) - KERNEL_VMA == __kernel_phys_end, "bss leaves a page tail")

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note .note.*) }
}

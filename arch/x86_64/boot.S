/*
 * VOS x86_64 boot path.
 *
 * GRUB loads this ELF64 image via Multiboot2 and enters _start in 32-bit
 * protected mode with paging off, EAX = Multiboot2 magic and EBX = physical
 * address of the boot information. The 32-bit stub validates the handoff,
 * checks for long mode, builds the boot page tables, enables long mode and
 * jumps to the higher-half kernel.
 *
 * Boot page tables (2 MiB pages, one shared page directory):
 *   0x0000000000000000-0x000000003fffffff -> phys 0-1 GiB  identity, boot only
 *   0xffffffff80000000-0xffffffffbfffffff -> phys 0-1 GiB  kernel (KERNEL_VMA)
 * The boot GDT stays in the identity-mapped area until a later stage
 * replaces it. These mappings are writable and executable; kernel_main
 * removes the identity map and kmap.c replaces the kernel directory with the
 * W^X kernel map before anything else runs (N2.0).
 */

.set KERNEL_VMA,        0xFFFFFFFF80000000
.set KERNEL_PML4_INDEX, (KERNEL_VMA >> 39) & 511     /* 511 */
.set KERNEL_PDPT_INDEX, (KERNEL_VMA >> 30) & 511     /* 510 */

.set MB2_HEADER_MAGIC,  0xE85250D6
.set MB2_ARCH_I386,     0
.set MB2_BOOT_MAGIC,    0x36D76289

.set BOOT_CS,           0x08
.set BOOT_DS,           0x10
.set COM1,              0x3F8
.set VGA_TEXT,          0xB8000
.set MSR_EFER,          0xC0000080
.set EFER_LME,          1 << 8
.set CR4_PAE,           1 << 5
.set CR0_PG,            1 << 31
.set EFLAGS_ID,         1 << 21
.set CPUID_EXT_LM,      1 << 29
.set PTE_PRESENT_RW,    0x003
.set PDE_PRESENT_RW_2M, 0x083

.set TEST_EXIT_PORT,    0xF4
.set TEST_EXIT_FAIL,    0x11        /* QEMU exit status (0x11 << 1) | 1 = 35 */

/* Multiboot2 header: 8-byte aligned, within the first 32 KiB of the file. */
.section .multiboot2, "a"
.align 8
mb2_header:
    .long MB2_HEADER_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header
    .long 0x100000000 - (MB2_HEADER_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))
    .short 6                        /* module alignment tag: type */
    .short 0                        /*                       flags */
    .long 8                         /*                       size */
    .short 0                        /* end tag: type */
    .short 0                        /*          flags */
    .long 8                         /*          size */
mb2_header_end:

/* ---------------- 32-bit stub, runs at its physical address ---------------- */
.section .boot.text, "ax"
.code32
.global _start
.type _start, @function
_start:
    cli
    cld
    mov %eax, boot_mb2_magic
    mov %ebx, boot_mb2_info

    /* Zero .boot.bss (page tables, boot stack) before first use. */
    mov $__boot_bss_start, %edi
    mov $__boot_bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx
    xor %eax, %eax
    rep stosl
    mov $boot_stack_top, %esp

    mov $msg_bad_magic, %esi
    cmpl $MB2_BOOT_MAGIC, boot_mb2_magic
    jne boot_fatal

    /* CPUID exists if EFLAGS.ID can be toggled. */
    mov $msg_no_long_mode, %esi
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $EFLAGS_ID, %eax
    push %eax
    popfl
    pushfl
    pop %eax
    push %ecx
    popfl
    cmp %eax, %ecx
    je boot_fatal

    /* Long mode: CPUID 0x80000001 EDX bit 29. */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb boot_fatal
    mov $0x80000001, %eax
    cpuid
    test $CPUID_EXT_LM, %edx
    jz boot_fatal

    /* Page tables (already zeroed with .boot.bss). */
    mov $(boot_pdpt_low + PTE_PRESENT_RW), %eax
    mov %eax, boot_pml4
    mov $(boot_pdpt_high + PTE_PRESENT_RW), %eax
    mov %eax, boot_pml4 + KERNEL_PML4_INDEX * 8
    mov $(boot_pd + PTE_PRESENT_RW), %eax
    mov %eax, boot_pdpt_low
    mov %eax, boot_pdpt_high + KERNEL_PDPT_INDEX * 8

    xor %ecx, %ecx                  /* 512 x 2 MiB = phys 0-1 GiB */
1:  mov %ecx, %eax
    shl $21, %eax
    or $PDE_PRESENT_RW_2M, %eax
    mov %eax, boot_pd(, %ecx, 8)
    inc %ecx
    cmp $512, %ecx
    jb 1b

    /* Enter long mode: PAE, EFER.LME, then paging. */
    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $CR4_PAE, %eax
    mov %eax, %cr4
    mov $MSR_EFER, %ecx
    rdmsr
    or $EFER_LME, %eax
    wrmsr
    mov %cr0, %eax
    or $CR0_PG, %eax
    mov %eax, %cr0

    lgdt boot_gdt_ptr
    ljmp $BOOT_CS, $long_mode_entry

/* Print the string at %esi on COM1 and VGA row 0, then halt. */
boot_fatal:
    mov $serial_init_table, %ebx
1:  movzbl (%ebx), %edx
    add $COM1, %edx
    movb 1(%ebx), %al
    out %al, %dx
    add $2, %ebx
    cmp $serial_init_table_end, %ebx
    jb 1b

    mov $VGA_TEXT, %edi
    mov $0x0720, %ax
    mov $(80 * 25), %ecx
    rep stosw
    mov $VGA_TEXT, %edi
2:  lodsb
    test %al, %al
    jz 5f
    cmp $0x20, %al                  /* control characters go to serial only */
    jb 3f
    mov $0x4F, %ah                  /* white on red */
    mov %ax, (%edi)
    add $2, %edi
3:  mov %al, %bl
    mov $(COM1 + 5), %dx
4:  in %dx, %al
    test $0x20, %al
    jz 4b
    mov %bl, %al
    mov $COM1, %dx
    out %al, %dx
    jmp 2b
5:
#ifdef VOS_TEST
    mov $TEST_EXIT_PORT, %dx
    mov $TEST_EXIT_FAIL, %eax
    out %eax, %dx
#endif
6:  hlt
    jmp 6b
.size _start, . - _start

.code64
long_mode_entry:
    mov $BOOT_DS, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    movabs $higher_half_entry, %rax
    jmp *%rax

.section .boot.rodata, "a"
serial_init_table:                  /* (COM1 register offset, value) */
    .byte 1, 0x00                   /* IER: interrupts off */
    .byte 3, 0x80                   /* LCR: DLAB on */
    .byte 0, 0x03                   /* DLL: divisor 3 = 38400 baud */
    .byte 1, 0x00                   /* DLM */
    .byte 3, 0x03                   /* LCR: 8N1, DLAB off */
    .byte 2, 0xC7                   /* FCR: enable and clear FIFOs */
    .byte 4, 0x03                   /* MCR: DTR, RTS */
serial_init_table_end:
msg_bad_magic:
    .asciz "VOS: boot error: not started by a Multiboot2 loader\r\n"
msg_no_long_mode:
    .asciz "VOS: boot error: CPU lacks long mode (x86-64), halting\r\n"

.section .boot.data, "aw"
.align 8
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF        /* BOOT_CS: 64-bit code, DPL 0 */
    .quad 0x00CF92000000FFFF        /* BOOT_DS: data, DPL 0 */
boot_gdt_end:
boot_gdt_ptr:
    .short boot_gdt_end - boot_gdt - 1
    .long boot_gdt
.align 4
boot_mb2_magic: .long 0
boot_mb2_info:  .long 0

.section .boot.bss, "aw", @nobits
.align 4096
boot_pml4:      .skip 4096
boot_pdpt_low:  .skip 4096
boot_pdpt_high: .skip 4096
boot_pd:        .skip 4096
boot_stack:     .skip 4096
boot_stack_top:

/* ---------------- 64-bit, higher half ---------------- */
.section .text
.code64
higher_half_entry:
    /* Zero the kernel .bss (includes the kernel stack) before running C. */
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    sub %rdi, %rcx
    shr $3, %rcx
    xor %eax, %eax
    rep stosq

    movabs $kernel_stack_top, %rsp
    xor %ebp, %ebp
    /* Read the saved handoff through the higher-half alias of .boot.data. */
    movl boot_mb2_magic + KERNEL_VMA, %edi
    movl boot_mb2_info + KERNEL_VMA, %esi
    call kernel_main
1:  cli
    hlt
    jmp 1b

/*
 * The stack kernel_main runs on, with an unmapped guard page below it once
 * kmap.c has switched to the W^X kernel map (N2.0): an overflow faults
 * instead of overwriting .bss.
 */
.section .bss.kstack, "aw", @nobits
.balign 4096
.global boot_kstack_guard, kernel_stack, kernel_stack_top
boot_kstack_guard:  .skip 4096
kernel_stack:       .skip 16384
kernel_stack_top:

.section .note.GNU-stack, "", @progbits

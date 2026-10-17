# i386: 32-bit protected-mode kernel loaded by GRUB via Multiboot 1.

ARCH_CFLAGS     := -m32
ARCH_ASFLAGS    := -m32
ARCH_LDFLAGS    := -m elf_i386
ARCH_OBJS       := boot.o kernel.o
QEMU            := qemu-system-i386
MULTIBOOT_CHECK := --is-x86-multiboot

# Headless boot; passes only on the expected serial output (see scripts/test.sh).
arch-test: iso
	scripts/test.sh $(ISO) $(BUILD)/serial.log

# MyOS top-level build. Select the target with ARCH=<name> (default: i386).
# Each architecture lives in arch/<name>/ and provides an arch.mk fragment.

ARCH  ?= i386
ARCHS := i386

.DEFAULT_GOAL := all

ifeq ($(filter $(ARCH),$(ARCHS)),)
$(error Unsupported ARCH '$(ARCH)'; choose one of: $(ARCHS))
endif

ARCH_DIR := arch/$(ARCH)
BUILD    := build/$(ARCH)
KERNEL   := $(BUILD)/myos.bin
ISO      := $(BUILD)/myos.iso

CC     := gcc
LD     := ld
CFLAGS := -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-builtin -fno-asynchronous-unwind-tables -mno-sse -mno-mmx \
          -O2 -Wall -Wextra

# Provides ARCH_CFLAGS, ARCH_ASFLAGS, ARCH_LDFLAGS, ARCH_OBJS, QEMU,
# MULTIBOOT_CHECK and the arch-test recipe.
include $(ARCH_DIR)/arch.mk

OBJS := $(addprefix $(BUILD)/,$(ARCH_OBJS))

.PHONY: all verify iso run test arch-test clean
all: $(KERNEL)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: $(ARCH_DIR)/%.S Makefile $(ARCH_DIR)/arch.mk | $(BUILD)
	$(CC) $(ARCH_ASFLAGS) -c $< -o $@

$(BUILD)/%.o: $(ARCH_DIR)/%.c Makefile $(ARCH_DIR)/arch.mk | $(BUILD)
	$(CC) $(CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) $(ARCH_DIR)/linker.ld
	$(LD) $(ARCH_LDFLAGS) -nostdlib -T $(ARCH_DIR)/linker.ld -o $@ $(OBJS)

verify: $(KERNEL)
	grub-file $(MULTIBOOT_CHECK) $(KERNEL)
	@echo "$(ARCH): Multiboot header OK"

iso: verify
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/myos.bin
	cp $(ARCH_DIR)/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

run: iso
	$(QEMU) -cdrom $(ISO)

# Headless QEMU tests for the selected ARCH; see arch/<name>/arch.mk.
test: arch-test

clean:
	rm -rf build

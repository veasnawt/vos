# VOS top-level build. Select the target with ARCH=<name> (default: i386).
# Each architecture lives in arch/<name>/ and provides an arch.mk fragment.

ARCH  ?= i386
ARCHS := i386 x86_64

.DEFAULT_GOAL := all

ifeq ($(filter $(ARCH),$(ARCHS)),)
$(error Unsupported ARCH '$(ARCH)'; choose one of: $(ARCHS))
endif

ARCH_DIR := arch/$(ARCH)
BUILD    := build/$(ARCH)
TEST_ISO := build/$(ARCH)-test/vos.iso

# VOS_TEST=1 builds the self-checking test image into build/<ARCH>-test/.
# MYOS_TEST is the pre-rename name; reject it rather than silently
# building the production image.
ifdef MYOS_TEST
$(error MYOS_TEST was renamed to VOS_TEST)
endif
ifeq ($(VOS_TEST),1)
BUILD     := build/$(ARCH)-test
TEST_DEFS := -DVOS_TEST
# VOS_TEST_CASE=<name> compiles in the test case "name" (-DVOS_TEST_CASE_<name>)
# and builds into its own directory so it never replaces the main test image.
ifneq ($(VOS_TEST_CASE),)
BUILD     := build/$(ARCH)-test-$(VOS_TEST_CASE)
TEST_DEFS += -DVOS_TEST_CASE_$(VOS_TEST_CASE)
endif
else ifneq ($(VOS_TEST_CASE),)
$(error VOS_TEST_CASE requires VOS_TEST=1)
endif

KERNEL   := $(BUILD)/vos.bin
ISO      := $(BUILD)/vos.iso

CC     := gcc
LD     := ld
CFLAGS := -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-builtin -fno-asynchronous-unwind-tables -mno-sse -mno-mmx \
          -O2 -Wall -Wextra

# Provides ARCH_CFLAGS, ARCH_ASFLAGS, ARCH_LDFLAGS, ARCH_OBJS, QEMU,
# MULTIBOOT_CHECK and the arch-test recipe (which may use TEST_ISO).
include $(ARCH_DIR)/arch.mk

OBJS := $(addprefix $(BUILD)/,$(ARCH_OBJS))

.PHONY: all verify iso run test test-all arch-test clean
all: $(KERNEL)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: $(ARCH_DIR)/%.S Makefile $(ARCH_DIR)/arch.mk | $(BUILD)
	$(CC) $(ARCH_ASFLAGS) $(TEST_DEFS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: $(ARCH_DIR)/%.c Makefile $(ARCH_DIR)/arch.mk | $(BUILD)
	$(CC) $(CFLAGS) $(ARCH_CFLAGS) $(TEST_DEFS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJS) $(ARCH_DIR)/linker.ld
	$(LD) $(ARCH_LDFLAGS) -nostdlib -T $(ARCH_DIR)/linker.ld -o $@ $(OBJS)

verify: $(KERNEL)
	grub-file $(MULTIBOOT_CHECK) $(KERNEL)
	@echo "$(ARCH): Multiboot header OK"

iso: verify
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/vos.bin
	cp $(ARCH_DIR)/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

run: iso
	$(QEMU) -cdrom $(ISO)

# Headless QEMU tests for the selected ARCH; see arch/<name>/arch.mk.
test: arch-test

test-all:
	@set -e; for a in $(ARCHS); do $(MAKE) --no-print-directory ARCH=$$a test; done

clean:
	rm -rf build

-include $(OBJS:.o=.d)

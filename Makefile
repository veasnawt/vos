# VOS top-level build. Select the target with ARCH=<name> (default: x86_64).
# Each architecture lives in arch/<name>/ and provides an arch.mk fragment.

ARCH  ?= x86_64
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
TEST_DEFS += -DVOS_TEST_CASE_$(subst -,_,$(VOS_TEST_CASE))
endif
else ifneq ($(VOS_TEST_CASE),)
$(error VOS_TEST_CASE requires VOS_TEST=1)
endif

# Per-image GRUB configuration and extra ISO files ("source:path-in-iso").
# Test cases may override them in tests/<ARCH>/<case>.mk. arch.mk may add
# ARCH_ISO_FILES, which every image of that architecture carries.
GRUB_CFG  := $(ARCH_DIR)/grub.cfg
ISO_FILES :=
ifneq ($(VOS_TEST_CASE),)
-include tests/$(ARCH)/$(VOS_TEST_CASE).mk
endif

KERNEL   := $(BUILD)/vos.bin
ISO      := $(BUILD)/vos.iso

CC     := gcc
LD     := ld
CFLAGS := -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-builtin -fno-asynchronous-unwind-tables -mno-sse -mno-mmx \
          -O2 -Wall -Wextra

# Provides ARCH_CFLAGS, ARCH_ASFLAGS, ARCH_LDFLAGS, ARCH_OBJS, QEMU,
# MULTIBOOT_CHECK and the arch-test recipe (which may use TEST_ISO); optionally
# ARCH_LINK_CHECK, run on the kernel right after it is linked.
include $(ARCH_DIR)/arch.mk

OBJS := $(addprefix $(BUILD)/,$(ARCH_OBJS))

# x86_64 has a user space (runtime and applications), built separately.
ifeq ($(ARCH),x86_64)
include user/user.mk
endif

.PHONY: all verify iso uefi-iso run debug test test-all arch-test test-uefi clean
all: $(KERNEL)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: $(ARCH_DIR)/%.S Makefile $(ARCH_DIR)/arch.mk | $(BUILD)
	$(CC) $(ARCH_ASFLAGS) $(TEST_DEFS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: $(ARCH_DIR)/%.c Makefile $(ARCH_DIR)/arch.mk | $(BUILD)
	$(CC) $(CFLAGS) $(ARCH_CFLAGS) $(TEST_DEFS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJS) $(ARCH_DIR)/linker.ld
	$(LD) $(ARCH_LDFLAGS) -nostdlib -T $(ARCH_DIR)/linker.ld -o $@ $(OBJS)
	$(ARCH_LINK_CHECK)

verify: $(KERNEL)
	grub-file $(MULTIBOOT_CHECK) $(KERNEL)
	@echo "$(ARCH): Multiboot header OK"

iso: verify $(foreach f,$(ARCH_ISO_FILES) $(ISO_FILES),$(firstword $(subst :, ,$(f))))
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/vos.bin
	cp $(GRUB_CFG) $(BUILD)/isodir/boot/grub/grub.cfg
	for f in $(ARCH_ISO_FILES) $(ISO_FILES); do \
	    mkdir -p $(BUILD)/isodir/$$(dirname $${f#*:}); cp $${f%%:*} $(BUILD)/isodir/$${f#*:}; \
	done
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

# U1: the same kernel and modules as `iso`, on a UEFI-only GRUB x86_64-efi ISO
# (EFI System Partition with \EFI\BOOT\BOOTX64.EFI). UEFI_HEAD is prepended to
# the image's GRUB configuration; UEFI_TAG names variants. Needs GRUB's
# x86_64-efi files (GRUB_EFI_DIR, grub-efi-amd64-bin) and mtools on PATH; it
# writes only into $(BUILD) (no disk, partition or firmware variable).
GRUB_EFI_DIR ?= /usr/lib/grub/x86_64-efi
UEFI_HEAD    ?= $(ARCH_DIR)/grub-uefi-head.cfg
UEFI_TAG     ?=
UEFI_ISO     := $(BUILD)/vos-uefi$(UEFI_TAG).iso
uefi-iso: iso
	@test "$(ARCH)" = x86_64 || { echo "uefi-iso: x86_64 only"; exit 1; }
	@test -f $(GRUB_EFI_DIR)/kernel.img -a -f $(GRUB_EFI_DIR)/multiboot2.mod || \
	    { echo "uefi-iso: no GRUB x86_64-efi files in $(GRUB_EFI_DIR) (see docs/plans/uefi.md)"; exit 1; }
	@command -v mformat >/dev/null || { echo "uefi-iso: mtools (mformat) not on PATH"; exit 1; }
	rm -rf $(BUILD)/isodir-uefi$(UEFI_TAG)
	cp -r $(BUILD)/isodir $(BUILD)/isodir-uefi$(UEFI_TAG)
	cat $(UEFI_HEAD) $(GRUB_CFG) > $(BUILD)/isodir-uefi$(UEFI_TAG)/boot/grub/grub.cfg
	grub-mkrescue -d $(GRUB_EFI_DIR) -o $(UEFI_ISO) $(BUILD)/isodir-uefi$(UEFI_TAG)

run: iso
	$(QEMU) -cdrom $(ISO)

# N4 first input is opt-in; keep the normal boot/test configurations intact.
# All generated files, including the user runtime, use a separate build tree.
INPUT_BUILD ?= build/n4-input
.PHONY: input-iso input-uefi-iso input-run
input-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(INPUT_BUILD)/kernel USER_BUILD=$(INPUT_BUILD)/user GRUB_CFG=arch/x86_64/grub-input.cfg iso
input-uefi-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(INPUT_BUILD)/kernel USER_BUILD=$(INPUT_BUILD)/user GRUB_CFG=arch/x86_64/grub-input.cfg uefi-iso
input-run: input-iso
	qemu-system-x86_64 -m 128M -cdrom $(INPUT_BUILD)/kernel/vos.iso -serial stdio -no-reboot -nic none

VSH_BUILD ?= build/n4-vsh
ifneq ($(filter vsh-iso vsh-uefi-iso vsh-run,$(MAKECMDGOALS)),)
ifeq ($(strip $(VSH_BUILD)),)
$(error VSH_BUILD must be nonempty; use a path under build/)
endif
endif
VSH_DISK ?=
VSH_DISK_ARGS = $(if $(VSH_DISK),-drive file=$(VSH_DISK)$(comma)if=none$(comma)id=vs0$(comma)format=raw$(comma)readonly=on -device virtio-blk-pci$(comma)drive=vs0$(comma)disable-legacy=on)
comma := ,
.PHONY: vsh-iso vsh-uefi-iso vsh-run
vsh-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 USER_BUILD=$(VSH_BUILD)/user user-vsh
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(VSH_BUILD)/kernel USER_BUILD=$(VSH_BUILD)/user GRUB_CFG=arch/x86_64/grub-vsh.cfg ISO_FILES=$(VSH_BUILD)/user/vsh.elf:boot/vsh.elf iso
vsh-uefi-iso: vsh-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(VSH_BUILD)/kernel USER_BUILD=$(VSH_BUILD)/user GRUB_CFG=arch/x86_64/grub-vsh.cfg ISO_FILES=$(VSH_BUILD)/user/vsh.elf:boot/vsh.elf uefi-iso
vsh-run: vsh-iso
	qemu-system-x86_64 -m 128M -cdrom $(VSH_BUILD)/kernel/vos.iso -serial stdio -no-reboot -nic none $(VSH_DISK_ARGS)

RUST_BUILD ?= build/n4-rust
RUST_APP ?= rust-demo
ifneq ($(filter rust-app rust-demo-iso rust-demo-uefi-iso rust-demo-run,$(MAKECMDGOALS)),)
ifeq ($(strip $(RUST_BUILD)),)
$(error RUST_BUILD must be nonempty; use a path under build/)
endif
ifeq ($(strip $(RUST_APP)),)
$(error RUST_APP must name an application under user/)
endif
endif
.PHONY: rust-app rust-demo-iso rust-demo-uefi-iso rust-demo-run
rust-app:
	$(MAKE) --no-print-directory ARCH=x86_64 USER_BUILD=$(RUST_BUILD)/user RUST_APP=$(RUST_APP) user-rust
rust-demo-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 USER_BUILD=$(RUST_BUILD)/user RUST_APP=rust-demo user-rust
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(RUST_BUILD)/kernel USER_BUILD=$(RUST_BUILD)/user GRUB_CFG=arch/x86_64/grub-rust-demo.cfg ISO_FILES=$(RUST_BUILD)/user/rust-demo.elf:boot/rust-demo.elf iso
rust-demo-uefi-iso: rust-demo-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(RUST_BUILD)/kernel USER_BUILD=$(RUST_BUILD)/user GRUB_CFG=arch/x86_64/grub-rust-demo.cfg ISO_FILES=$(RUST_BUILD)/user/rust-demo.elf:boot/rust-demo.elf uefi-iso
rust-demo-run: rust-demo-iso
	qemu-system-x86_64 -m 128M -cdrom $(RUST_BUILD)/kernel/vos.iso -serial stdio -no-reboot -nic none

RIXIE_BUILD ?= build/n4-rixie
ifneq ($(filter rixie-iso rixie-uefi-iso rixie-run,$(MAKECMDGOALS)),)
ifeq ($(strip $(RIXIE_BUILD)),)
$(error RIXIE_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: rixie-iso rixie-uefi-iso rixie-run
rixie-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 USER_BUILD=$(RIXIE_BUILD)/user RUST_APP=rixie user-rust
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(RIXIE_BUILD)/kernel USER_BUILD=$(RIXIE_BUILD)/user GRUB_CFG=arch/x86_64/grub-rixie.cfg ISO_FILES=$(RIXIE_BUILD)/user/rixie.elf:boot/rixie.elf iso
rixie-uefi-iso: rixie-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(RIXIE_BUILD)/kernel USER_BUILD=$(RIXIE_BUILD)/user GRUB_CFG=arch/x86_64/grub-rixie.cfg ISO_FILES=$(RIXIE_BUILD)/user/rixie.elf:boot/rixie.elf uefi-iso
rixie-run: rixie-iso
	qemu-system-x86_64 -m 128M -cdrom $(RIXIE_BUILD)/kernel/vos.iso -serial stdio -no-reboot -nic none

NET_BUILD ?= build/n5-net
ifneq ($(filter net-iso net-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(NET_BUILD)),)
$(error NET_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: net-iso net-uefi-iso
net-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(NET_BUILD)/kernel USER_BUILD=$(NET_BUILD)/user GRUB_CFG=arch/x86_64/grub-net.cfg iso
net-uefi-iso: net-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(NET_BUILD)/kernel USER_BUILD=$(NET_BUILD)/user GRUB_CFG=arch/x86_64/grub-net.cfg uefi-iso

ARP_BUILD ?= build/n5-arp
ifneq ($(filter arp-iso arp-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(ARP_BUILD)),)
$(error ARP_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: arp-iso arp-uefi-iso
arp-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(ARP_BUILD)/kernel USER_BUILD=$(ARP_BUILD)/user GRUB_CFG=arch/x86_64/grub-arp.cfg iso
arp-uefi-iso: arp-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(ARP_BUILD)/kernel USER_BUILD=$(ARP_BUILD)/user GRUB_CFG=arch/x86_64/grub-arp.cfg uefi-iso

IPV4_BUILD ?= build/n5-ipv4
ifneq ($(filter ipv4-iso ipv4-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(IPV4_BUILD)),)
$(error IPV4_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: ipv4-iso ipv4-uefi-iso
ipv4-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(IPV4_BUILD)/kernel USER_BUILD=$(IPV4_BUILD)/user GRUB_CFG=arch/x86_64/grub-ipv4.cfg iso
ipv4-uefi-iso: ipv4-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(IPV4_BUILD)/kernel USER_BUILD=$(IPV4_BUILD)/user GRUB_CFG=arch/x86_64/grub-ipv4.cfg uefi-iso

UDP_BUILD ?= build/n5-udp
ifneq ($(filter udp-iso udp-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(UDP_BUILD)),)
$(error UDP_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: udp-iso udp-uefi-iso
udp-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(UDP_BUILD)/kernel USER_BUILD=$(UDP_BUILD)/user GRUB_CFG=arch/x86_64/grub-udp.cfg iso
udp-uefi-iso: udp-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(UDP_BUILD)/kernel USER_BUILD=$(UDP_BUILD)/user GRUB_CFG=arch/x86_64/grub-udp.cfg uefi-iso

TCP_BUILD ?= build/n5-tcp
ifneq ($(filter tcp-iso tcp-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(TCP_BUILD)),)
$(error TCP_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: tcp-iso tcp-uefi-iso
tcp-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_BUILD)/kernel USER_BUILD=$(TCP_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp.cfg iso
tcp-uefi-iso: tcp-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_BUILD)/kernel USER_BUILD=$(TCP_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp.cfg uefi-iso

TCP_DATA_BUILD ?= build/n5-tcp-data
ifneq ($(filter tcp-data-iso tcp-data-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(TCP_DATA_BUILD)),)
$(error TCP_DATA_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: tcp-data-iso tcp-data-uefi-iso
tcp-data-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_DATA_BUILD)/kernel USER_BUILD=$(TCP_DATA_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp-data.cfg iso
tcp-data-uefi-iso: tcp-data-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_DATA_BUILD)/kernel USER_BUILD=$(TCP_DATA_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp-data.cfg uefi-iso

TCP_RETRY_BUILD ?= build/n5-tcp-retry
ifneq ($(filter tcp-retry-iso tcp-retry-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(TCP_RETRY_BUILD)),)
$(error TCP_RETRY_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: tcp-retry-iso tcp-retry-uefi-iso
tcp-retry-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_RETRY_BUILD)/kernel USER_BUILD=$(TCP_RETRY_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp-retry.cfg iso
tcp-retry-uefi-iso: tcp-retry-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_RETRY_BUILD)/kernel USER_BUILD=$(TCP_RETRY_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp-retry.cfg uefi-iso

# Like run, but QEMU waits for a debugger on TCP port 1234 (-s -S) and prints
# the serial port here. GDB is not installed by the build; see
# docs/plans/n2-foundation.md (tooling) for the commands.
debug: iso
	$(QEMU) -cdrom $(ISO) -serial stdio -display none -no-reboot -s -S

# Headless QEMU tests for the selected ARCH; see arch/<name>/arch.mk.
test: arch-test

# U1: the UEFI test matrix under QEMU q35 + OVMF (x86_64; see arch/x86_64/arch.mk).
test-uefi: arch-test-uefi

test-all:
	@set -e; for a in $(ARCHS); do $(MAKE) --no-print-directory ARCH=$$a test; done

clean:
	rm -rf build

-include $(OBJS:.o=.d)

TCP_CLOSE_BUILD ?= build/n5-tcp-close
ifneq ($(filter tcp-close-iso tcp-close-uefi-iso,$(MAKECMDGOALS)),)
ifeq ($(strip $(TCP_CLOSE_BUILD)),)
$(error TCP_CLOSE_BUILD must be nonempty; use a path under build/)
endif
endif
.PHONY: tcp-close-iso tcp-close-uefi-iso
tcp-close-iso:
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_CLOSE_BUILD)/kernel USER_BUILD=$(TCP_CLOSE_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp-close.cfg iso
tcp-close-uefi-iso: tcp-close-iso
	$(MAKE) --no-print-directory ARCH=x86_64 BUILD=$(TCP_CLOSE_BUILD)/kernel USER_BUILD=$(TCP_CLOSE_BUILD)/user GRUB_CFG=arch/x86_64/grub-tcp-close.cfg uefi-iso

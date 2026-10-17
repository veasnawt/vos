# x86_64: ELF64 kernel loaded by GRUB via Multiboot2. boot.S switches from
# 32-bit protected mode to long mode and jumps to the higher half.

# -fcf-protection=none and -fno-stack-clash-protection override Ubuntu gcc
# defaults (endbr64 instrumentation, stack probes) that a kernel must not inherit.
ARCH_CFLAGS     := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
                   -fcf-protection=none -fno-stack-clash-protection
ARCH_ASFLAGS    := -m64
ARCH_LDFLAGS    := -m elf_x86_64 -z max-page-size=0x1000
ARCH_OBJS       := boot.o kernel.o
QEMU            := qemu-system-x86_64
MULTIBOOT_CHECK := --is-x86-multiboot2

# 1. Production image (halts after boot): serial smoke test.
# 2. Test image (VOS_TEST=1, reports via isa-debug-exit): exit status AND
#    serial marker must both match; also run with 2 GiB and on a CPU
#    without long mode, which must fail cleanly with status 35.
arch-test: iso
	QEMU=$(QEMU) FAIL_PATTERN='FAILED|check failed|boot error' \
	    scripts/test.sh $(ISO) $(BUILD)/serial.log 'Hello from VOS!' 'boot: checks passed'
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 iso
	scripts/exit-test.sh boot-128M $(TEST_ISO) 33 'VOS-TEST: PASS'
	MEM=2G scripts/exit-test.sh boot-2G $(TEST_ISO) 33 'VOS-TEST: PASS'
	scripts/exit-test.sh no-long-mode $(TEST_ISO) 35 'CPU lacks long mode' -cpu qemu32

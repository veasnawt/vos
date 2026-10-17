# x86_64: ELF64 kernel loaded by GRUB via Multiboot2. boot.S switches from
# 32-bit protected mode to long mode and jumps to the higher half.

# -fcf-protection=none and -fno-stack-clash-protection override Ubuntu gcc
# defaults (endbr64 instrumentation, stack probes) that a kernel must not inherit.
ARCH_CFLAGS     := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
                   -fcf-protection=none -fno-stack-clash-protection
ARCH_ASFLAGS    := -m64
ARCH_LDFLAGS    := -m elf_x86_64 -z max-page-size=0x1000
ARCH_OBJS       := boot.o entry.o kernel.o bootinfo.o console.o cpu.o exception.o gdt.o idt.o \
                   lib.o panic.o pmm.o resv.o
ifeq ($(VOS_TEST),1)
ARCH_OBJS       += selftest.o selftest_pmm.o
endif
QEMU            := qemu-system-x86_64
MULTIBOOT_CHECK := --is-x86-multiboot2

# 1. Production image (halts after boot): serial smoke test.
# 2. Test image (VOS_TEST=1, reports via isa-debug-exit): exit status AND
#    serial marker must both match; also run with 2 GiB and on a CPU
#    without long mode, which must fail cleanly with status 35.
# 3. Test case images (VOS_TEST_CASE=<name>), each in its own build directory:
#    panic, kernel-ud and kernel-df must exit with status 35 and report it;
#    modules and pmm-exhaust must pass (status 33).
# Every test image runs the bootinfo and pmm self-tests; any failure turns
# VOS-TEST: PASS (33) into VOS-TEST: FAIL (35).
arch-test: iso
	QEMU=$(QEMU) FAIL_PATTERN='FAILED|check failed|boot error' \
	    scripts/test.sh $(ISO) $(BUILD)/serial.log 'Hello from VOS!' 'boot: checks passed'
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 iso
	scripts/exit-test.sh boot-128M $(TEST_ISO) 33 'selftest: pmm-synthetic ' \
	    'selftest: pmm-kernel ' 'selftest: 1000 frames from ' 'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh boot-2G $(TEST_ISO) 33 'selftest: pmm-synthetic ' \
	    'selftest: pmm-kernel ' 'selftest: 1000 frames from ' 'VOS-TEST: PASS' --
	scripts/exit-test.sh no-long-mode $(TEST_ISO) 35 'CPU lacks long mode' -cpu qemu32
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=panic iso
	scripts/exit-test.sh panic build/$(ARCH)-test-panic/vos.iso 35 \
	    'panic: test case: deliberate panic' 'VOS-TEST: FAIL' --
	scripts/exit-test.sh gdt-tss $(TEST_ISO) 33 'gdt: loaded cs=0x8 ss=0x10 tr=0x28 tss=busy' \
	    'tss: rsp0=' 'idt: 32 exception vectors loaded, df ist=1' \
	    'cpu: wp=on nx=on' 'VOS-TEST: PASS' --
	scripts/exit-test.sh no-nx $(TEST_ISO) 35 'panic: CPU lacks NX' 'VOS-TEST: FAIL' -- -cpu qemu64,-nx
	scripts/exit-test.sh cpu-max $(TEST_ISO) 33 'smep=on umip=on' 'VOS-TEST: PASS' -- -cpu max
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-ud iso
	scripts/exit-test.sh kernel-ud build/$(ARCH)-test-kernel-ud/vos.iso 35 \
	    'exc: #UD vec=6 err=- cs=0x8 rip=' 'panic: #UD invalid opcode' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-df iso
	scripts/exit-test.sh kernel-df build/$(ARCH)-test-kernel-df/vos.iso 35 \
	    'exc: #DF vec=8 err=0x0 cs=0x8 rip=' 'stack=ist1' 'panic: #DF double fault' \
	    'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=modules iso
	scripts/exit-test.sh modules build/$(ARCH)-test-modules/vos.iso 33 \
	    'boot: kernel command line "' 'vos.test=1' 'boot: modules 2' 'boot: module 0 ' \
	    'boot: module 1 ' 'selftest: modules ' 'selftest: pmm-kernel ' 'VOS-TEST: PASS' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=pmm-exhaust iso
	scripts/exit-test.sh pmm-exhaust build/$(ARCH)-test-pmm-exhaust/vos.iso 33 \
	    'selftest: drained ' 'selftest: pmm-exhaust ' 'VOS-TEST: PASS' --

# x86_64: ELF64 kernel loaded by GRUB via Multiboot2. boot.S switches from
# 32-bit protected mode to long mode and jumps to the higher half.

# -fcf-protection=none and -fno-stack-clash-protection override Ubuntu gcc
# defaults (endbr64 instrumentation, stack probes) that a kernel must not inherit.
# -g adds debug sections only (never loaded): for GDB and addr2line.
ARCH_CFLAGS     := -g -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
                   -fcf-protection=none -fno-stack-clash-protection
ARCH_ASFLAGS    := -g -m64
ARCH_LDFLAGS    := -m elf_x86_64 -z max-page-size=0x1000 -Map=$(BUILD)/kernel.map
# After every link (N3.6): the PT_LOAD segments must tile the page-rounded kernel
# reservation, leaving no page tail a loader may use; a failing kernel is removed.
ARCH_LINK_CHECK  = python3 -I -B scripts/check-kernel-layout.py $@ || { rm -f $@; exit 1; }
# USB.2-C3b.2: every link also proves the flavour from the binary itself (below).
CONTAIN_FLAVOUR  = production
ARCH_OBJS       := boot.o entry.o usermode.o kernel.o bootinfo.o console.o cpu.o exception.o gdt.o idt.o \
                   lib.o loader.o panic.o pmm.o process.o resv.o syscall.o usercopy.o vmm.o kmap.o irq.o pic.o pit.o thread.o thread_switch.o channel.o elf.o app.o app_catalog.o app_payload.o exec.o \
                   pci.o xhci.o xhci_boot.o blockdev.o virtio_blk.o ext2.o io_scope.o file.o devmap.o screen.o desktop.o gui.o gui_verify.o font8x16.o keyboard.o input_console.o user_console.o vsh_boot.o virtio_net.o net_boot.o net_arp.o net_arp_boot.o net_ipv4.o net_ipv4_boot.o net_udp.o net_udp_boot.o net_tcp.o net_tcp_boot.o net_tcp_data_boot.o net_tcp_retry_boot.o net_tcp_close_boot.o
ARCH_OBJS += usb_resources.o xhci_qualify.o usb_dma.o efi_memory.o xhci_survey.o
ifeq ($(EXEC_VERIFY),1)
ARCH_OBJS += exec_verify.o
endif
ifeq ($(HANDOFF_VERIFY),1)
ARCH_OBJS += handoff_verify.o
endif
ifeq ($(VOS_TEST),1)
ARCH_OBJS       += selftest.o selftest_cpu.o selftest_elf.o selftest_ipc.o selftest_irq.o selftest_kmap.o selftest_mp.o selftest_sched.o selftest_stress.o selftest_pmm.o selftest_suite.o \
                   selftest_user.o selftest_vblk.o selftest_blk.o selftest_ext2.o selftest_vmm.o selftest_screen.o selftest_io_scope.o selftest_file.o storage_observe.o user_progs.o
ifeq ($(VOS_TEST_CASE),accounting)
ARCH_OBJS += selftest_accounting.o
endif
ifeq ($(VOS_TEST_CASE),storage-stress)
ARCH_OBJS += selftest_storage_stress.o storage_stress.o
endif
ifeq ($(VOS_TEST_CASE),storage-stress-b)
ARCH_OBJS += storage_stress_b.o storage_stress.o
endif
ifeq ($(VOS_TEST_CASE),storage-stress-c)
ARCH_OBJS += storage_stress_c.o storage_stress.o
endif
ifeq ($(VOS_TEST_CASE),storage-stress-d)
ARCH_OBJS += storage_stress_d.o storage_stress.o
endif
endif
QEMU            := qemu-system-x86_64

# USB.2-C3b.2: the controller-containment write path exists only in explicitly
# named QEMU test images. VOS_CONTAIN_TEST=1 refuses any build directory whose
# name lacks "contain-test", so production and physical-PC images (build/x86_64
# and every other default) can never carry it.
# Layered protection (a directory name alone is not trusted):
#  1. VOS_CONTAIN_TEST=1 and VOS_CONTAIN_TEST_ACK=QEMU-ONLY-EXPERIMENTAL must both be
#     given on the make command line; an environment or makefile value is refused,
#     so a stray exported variable can never turn the executor on;
#  2. BUILD must be a directory whose name contains "contain-test", and VOS_TEST /
#     VOS_TEST_CASE must not be combined with it;
#  3. only the kernel goals (all, verify, the image itself) are accepted: no iso,
#     uefi-iso, run or any other packaging target can be built in this mode;
#  4. every link then proves the flavour from the binary (check-contain-image.py):
#     a production link fails if any executor symbol, string or marker is present
#     (catching KERNEL_RUST_EXTRA_FLAGS, CFLAGS or stale-object smuggling), and the
#     test link fails unless the marker and the full executor are present;
#  5. the `iso` recipe re-checks the kernel it packages (see Makefile).
ifneq ($(origin VOS_CONTAIN_TEST),undefined)
ifneq ($(origin VOS_CONTAIN_TEST),command line)
$(error VOS_CONTAIN_TEST must be given on the make command line, not inherited from the environment or a makefile)
endif
ifneq ($(VOS_CONTAIN_TEST),1)
$(error VOS_CONTAIN_TEST must be 1 or unset)
endif
ifneq ($(origin VOS_CONTAIN_TEST_ACK),command line)
$(error VOS_CONTAIN_TEST=1 also requires VOS_CONTAIN_TEST_ACK=QEMU-ONLY-EXPERIMENTAL on the make command line)
endif
ifneq ($(VOS_CONTAIN_TEST_ACK),QEMU-ONLY-EXPERIMENTAL)
$(error VOS_CONTAIN_TEST_ACK must be exactly QEMU-ONLY-EXPERIMENTAL)
endif
ifeq ($(findstring contain-test,$(BUILD)),)
$(error VOS_CONTAIN_TEST=1 requires BUILD=<directory whose name contains "contain-test">)
endif
ifneq ($(VOS_TEST)$(VOS_TEST_CASE),)
$(error VOS_CONTAIN_TEST=1 cannot be combined with VOS_TEST or VOS_TEST_CASE)
endif
ifneq ($(filter-out all verify $(KERNEL),$(MAKECMDGOALS)),)
$(error VOS_CONTAIN_TEST=1 builds the QEMU-only kernel only: refused goals: $(filter-out all verify $(KERNEL),$(MAKECMDGOALS)))
endif
ARCH_CFLAGS += -DVOS_CONTAIN_TEST
CONTAIN_RUST_CFG := --cfg vos_contain_test
CONTAIN_FLAVOUR := test
else ifneq ($(VOS_CONTAIN_TEST_ACK),)
$(error VOS_CONTAIN_TEST_ACK without VOS_CONTAIN_TEST=1)
endif
ARCH_LINK_CHECK += && python3 -I -B scripts/check-contain-image.py --expect $(CONTAIN_FLAVOUR) $@ || { rm -f $@; exit 1; }

# R0-B: one freestanding Rust archive, separate from userspace and ARCH_OBJS.
# Retain GNU ld, the existing five-segment script and the post-link validator.
RUSTC ?= rustc
ARCH_LIBS := rust/libvos_kernel.a
KERNEL_RUST_SOURCE ?= arch/x86_64/rust/kernel_probe.rs
KERNEL_RUST_FLAGS = --edition=2024 --target=x86_64-unknown-none --crate-name vos_kernel \
                    --crate-type=staticlib -D warnings -C panic=abort -C opt-level=2 \
                    -C target-cpu=x86-64 -C code-model=kernel -C relocation-model=static \
                    -C no-redzone=yes -C force-unwind-tables=no -C lto=off \
                    -C embed-bitcode=no -C codegen-units=1 -C debuginfo=0 \
                    --remap-path-prefix=$(CURDIR)=. $(KERNEL_RUST_EXTRA_FLAGS) \
                    $(if $(and $(filter 1,$(VOS_TEST)),$(filter rust-panic,$(VOS_TEST_CASE))),--cfg vos_rust_panic_test) \
                    $(CONTAIN_RUST_CFG)
.PHONY: kernel-rust-inputs
kernel-rust-inputs:
$(BUILD)/rust/inputs: kernel-rust-inputs
	@mkdir -p $(dir $@)
	@set -eu; tmp=$$(mktemp "$@.XXXXXX"); trap 'rm -f "$$tmp"' 0 1 2 15; \
	  { printf '%s\n' '$(KERNEL_RUST_SOURCE)' '$(KERNEL_RUST_FLAGS)'; \
	    "$(RUSTC)" -vV; sha256sum "$$(command -v "$(RUSTC)")"; \
	    libdir=$$("$(RUSTC)" --print target-libdir --target x86_64-unknown-none); \
	    sha256sum "$$libdir"/libcore-*.rlib "$$libdir"/libcompiler_builtins-*.rlib; \
	  } > "$$tmp"; \
	  if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; fi
$(BUILD)/rust/libvos_kernel.a: $(KERNEL_RUST_SOURCE) $(BUILD)/rust/inputs Makefile arch/x86_64/arch.mk
	$(RUSTC) $(KERNEL_RUST_FLAGS) --emit=link,dep-info=$(BUILD)/rust/kernel.d -o $@ $(KERNEL_RUST_SOURCE)
-include $(BUILD)/rust/kernel.d

# The boot application, delivered by GRUB as a Multiboot2 module (grub.cfg).
ARCH_ISO_FILES   = $(USER_BUILD)/hello.elf:boot/hello.elf
MULTIBOOT_CHECK := --is-x86-multiboot2

# N3.1 virtio-blk test disk: 2051 deterministic sectors (scripts/mkvblkdisk.c),
# generated once into build/vblk/, checked against its known SHA-256 and made
# read-only. QEMU attaches it read-only; only the writable-device run uses a
# copy, whose hash is checked again afterwards. No host disk is ever attached.
VBLK_SECTORS := 2051
VBLK_DISK    := build/vblk/disk.img
VBLK_SHA256  := 075bb733d12497e0d78b9b2fb2a905a674d3f0a8b6d27a827b92ce9f7f4d0c77
VBLK_TOOL    := build/vblk/mkvblkdisk
VBLK_RW_COPY := build/vblk/disk-rw.img
# Modern-only device (PCI ID 0x1042), read-only backing file.
VBLK_RO      := -drive file=$(VBLK_DISK),if=none,id=vd0,format=raw,readonly=on \
                -device virtio-blk-pci,drive=vd0,disable-legacy=on
$(VBLK_TOOL): scripts/mkvblkdisk.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
$(VBLK_DISK): $(VBLK_TOOL)
	$(VBLK_TOOL) $@.tmp $(VBLK_SECTORS)
	echo "$(VBLK_SHA256)  $@.tmp" | sha256sum -c --quiet
	chmod a-w $@.tmp
	mv -f $@.tmp $@

# N3.3 ext2 fixtures: images made by mke2fs -d (and e2fsck -D) from generated source trees,
# each with a manifest written from the source tree (scripts/mkext2fixtures.sh). Every image is
# built twice and compared, checked by e2fsck, made read-only, and its SHA-256 recorded in
# SHA256SUMS, which is checked again after the runs.
EXT2_DIR     := build/ext2
EXT2_FIXGEN  := build/ext2-tools/fixgen
EXT2_STAMP   := $(EXT2_DIR)/SHA256SUMS
EXT2_IMAGES  := 1k 2k 4k htree nofeat empty
EXT2_DISK    := -drive file=$(EXT2_DIR)/ext2-1k.img,if=none,id=vd0,format=raw,readonly=on \
                -device virtio-blk-pci,drive=vd0,disable-legacy=on
$(EXT2_FIXGEN): scripts/fixgen.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
$(EXT2_STAMP): scripts/mkext2fixtures.sh $(EXT2_FIXGEN)
	scripts/mkext2fixtures.sh $(EXT2_DIR) $(EXT2_FIXGEN)
$(foreach i,$(EXT2_IMAGES),$(EXT2_DIR)/ext2-$(i).img $(EXT2_DIR)/ext2-$(i).manifest): $(EXT2_STAMP)
	@test -f $@

# 1. Production image (halts after boot): serial smoke test.
# 2. Test image (VOS_TEST=1, reports via isa-debug-exit): exit status AND
#    serial marker must both match; also run with 2 GiB and on a CPU
#    without long mode, which must fail cleanly with status 35.
# 3. Test case images (VOS_TEST_CASE=<name>), each in its own build directory:
#    panic, kernel-ud and kernel-df must exit with status 35 and report it;
#    modules and pmm-exhaust must pass (status 33); kernel-lowaccess,
#    vmm-stale-tlb, vmm-nx and vmm-readonly must end in a real #PF with the
#    expected error code and CR2 (status 35); app-ambiguous and app-reject
#    must refuse to run the GRUB-delivered application (status 35).
#    N2.0 fault cases (fault-case) are compared with scripts/check-fault.sh;
#    nmi-real injects a genuine NMI through the QEMU monitor.
#    N2.1: kernel-irq-vector (int $0x20 in ring 0) must panic; the irq image
#    runs separately built programs with IF = 1 under the PIT (endless loop
#    killed by the test tick budget, register preservation, IF/IOPL lock,
#    kernel IF = 0, sti) under qemu64, 2 GiB and -cpu max.
#    N2.2: thread-overflow (#DF on IST1, CR2 in the thread's guard page),
#    and sched-deadlock (lost-wakeup panic) must end in the expected failure
#    (status 35).
#    N2.5: the stress image runs mixed concurrent workloads (selftest_stress.c)
#    with scheduler, process and channel audits, exhaustion and recovery,
#    under qemu64, 2 GiB and -cpu max.
#    N2.4: the ipc image runs trusted scenarios of separately built ABI v1
#    programs talking over channels (selftest_ipc.c) under qemu64, 2 GiB and
#    -cpu max; a verdict printed by a program must never be accepted.
#    N2.3: the mp image runs separately built programs at the same time
#    (isolation, fairness, faults, exhaustion, reclamation) under qemu64,
#    2 GiB and -cpu max.
# 5. Isolation suite (N1.9): 41 separately built fixtures, each with its
#    expected outcome in the trusted GRUB configuration, run under qemu64,
#    2 GiB and -cpu max (SMEP/UMIP/SMAP on); SMEP and SMAP cases with -cpu
#    max must end in a supervisor #PF, and with qemu64 must show the access
#    succeeding (control).
# Every test image runs the bootinfo and pmm self-tests; any failure turns
# VOS-TEST: PASS (33) into VOS-TEST: FAIL (35).
# 4. User space (no QEMU): hello.elf is built and validated, the negative ELF
#    tests run, and no kernel image may contain the application (user-test).
# N2.0 isolated fault case: build VOS_TEST_CASE=$(1), require exit status 35,
# the kmap self-test line and the markers $(2), then compare the CPU-reported
# exc: line with the case's own "fault-test: expect" line (scripts/check-fault.sh).
define fault-case
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=$(1) iso
	scripts/exit-test.sh $(1) build/$(ARCH)-test-$(1)/vos.iso 35 'selftest: kmap ' $(2) 'VOS-TEST: FAIL' --
	scripts/check-fault.sh build/$(ARCH)-test-$(1)/$(1).serial.log
endef

arch-test: iso
	QEMU=$(QEMU) FAIL_PATTERN='FAILED|check failed|boot error|rejected|killed' \
	    scripts/test.sh $(ISO) $(BUILD)/serial.log 'Hello from VOS!' 'boot: checks passed' \
	    'elf: hello: ET_EXEC entry 0x400000' 'user[hello]: Hello from VOS!' \
	    'proc[hello]: exited status 0, all frames reclaimed' 'kmap: after the TLB flush the CPU refused' \
	    'vmm: audit passed: ' 'sched: 16 thread records, 15 guarded stacks of 16 KiB'
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 iso
	scripts/exit-test.sh boot-128M $(TEST_ISO) 33 'user[hello-asm]: Hello from VOS!' \
	    'user[hello-asm]: second call resumed' 'user[badptr]: ABCD' 'user[forge]: VOS-FORGED-MARKER' \
	    'user[forge]: VOS-TEST: PASS?' 'user[forge]: ?[2J???' 'user[hello]: Hello from VOS!' \
	    'proc[hello]: exited status 0, all frames reclaimed' 'selftest: elf ' \
	    ' corrupted executables rejected for the intended reason' 'selftest: pmm-synthetic ' \
	    'selftest: pmm-kernel ' 'selftest: 1000 frames from ' 'selftest: vmm ' 'selftest: user ' \
	    'selftest: kmap ' 'kmap: phys 0-4 MiB as 4 KiB pages: ' \
	    'vblk: no virtio-blk device on PCI bus 0; block storage unavailable' 'selftest: vblk ' 'selftest: blk ' \
	    'selftest: ext2 ' \
	    'kmap: after the TLB flush the CPU refused a text write' 'vmm: audit passed: ' \
	    'irq: 8259 master 0x20-0x27, slave 0x28-0x2f (cascade IRQ2); masks 0xfffe (IRQ0 only)' \
	    'pit: channel 0 mode 2, divisor 11932 (99.998 Hz)' 'selftest: irq ' \
	    'selftest: ring-3 timer interrupts ' 'proc: killed by the test tick budget after 20 timer ticks' \
	    'selftest: sched ' 'selftest: scheduler: ' 'selftest: round robin over ' 'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh boot-2G $(TEST_ISO) 33 'selftest: pmm-synthetic ' \
	    'selftest: pmm-kernel ' 'selftest: 1000 frames from ' 'selftest: vmm ' 'selftest: user ' \
	    'selftest: kmap ' 'VOS-TEST: PASS' --
	@echo "forge-check: a kernel marker printed only by a user program must NOT be found"
	! scripts/exit-test.sh forge-check $(TEST_ISO) 33 'VOS-FORGED-MARKER' 'VOS-TEST: PASS' -- >/dev/null 2>&1
	scripts/exit-test.sh no-long-mode $(TEST_ISO) 35 'CPU lacks long mode' -cpu qemu32
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=panic iso
	scripts/exit-test.sh panic build/$(ARCH)-test-panic/vos.iso 35 \
	    'panic: test case: deliberate panic' 'VOS-TEST: FAIL' --
	scripts/exit-test.sh gdt-tss $(TEST_ISO) 33 'gdt: loaded cs=0x8 ss=0x10 tr=0x28 tss=busy' \
	    'tss: rsp0=' 'idt: 32 exception vectors loaded, df ist=1' 'idt: vector 0x80 present, DPL 3 (system calls' \
	    'idt: IRQ vectors 0x20-0x2f present, DPL 0, interrupt gates' \
	    'cpu: wp=on nx=on' 'VOS-TEST: PASS' --
	scripts/exit-test.sh no-nx $(TEST_ISO) 35 'panic: CPU lacks NX' 'VOS-TEST: FAIL' -- -cpu qemu64,-nx
	scripts/exit-test.sh cpu-max $(TEST_ISO) 33 'smep=on umip=on smap=on' 'selftest: user ' 'selftest: kmap ' \
	    'VOS-TEST: PASS' \
	    -- -cpu max
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-ud iso
	scripts/exit-test.sh kernel-ud build/$(ARCH)-test-kernel-ud/vos.iso 35 \
	    'exc: #UD vec=6 err=- cs=0x8 rip=' 'panic: #UD invalid opcode' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-df iso
	scripts/exit-test.sh kernel-df build/$(ARCH)-test-kernel-df/vos.iso 35 \
	    'exc: #DF vec=8 err=0x0 cs=0x8 rip=' 'stack=ist1' 'panic: #DF double fault' \
	    'VOS-TEST: FAIL' --
	$(call fault-case,wx-text-write,'exc: #PF vec=14 err=0x3 cs=0x8' 'panic: #PF page fault')
	$(call fault-case,wx-rodata-write,'exc: #PF vec=14 err=0x3 cs=0x8' 'panic: #PF page fault')
	$(call fault-case,wx-data-exec,'exc: #PF vec=14 err=0x11 cs=0x8' 'panic: #PF page fault')
	$(call fault-case,wx-alias-exec,'exc: #PF vec=14 err=0x11 cs=0x8' 'panic: #PF page fault')
	$(call fault-case,wx-small-alias-exec,'exc: #PF vec=14 err=0x11 cs=0x8' 'panic: #PF page fault')
	$(call fault-case,wx-guard-read,'exc: #PF vec=14 err=0x0 cs=0x8' 'panic: #PF page fault')
	$(call fault-case,kstack-overflow,'exc: #DF vec=8 err=0x0 cs=0x8' 'stack=ist1' 'panic: #DF double fault')
	$(call fault-case,nmi-gate,'not a hardware NMI' 'exc: NMI vec=2 err=- cs=0x8' 'stack=ist2' \
	    'panic: NMI non-maskable interrupt')
	$(call fault-case,mc-gate,'not a machine check' 'exc: #MC vec=18 err=- cs=0x8' 'stack=ist3' \
	    'panic: #MC machine check')
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=nmi-real iso
	scripts/nmi-test.sh nmi-real build/$(ARCH)-test-nmi-real/vos.iso 35 'waiting for an NMI' \
	    'selftest: kmap ' 'exc: NMI vec=2 err=- cs=0x8' 'stack=ist2' 'panic: NMI non-maskable interrupt' \
	    'VOS-TEST: FAIL' --
	scripts/check-fault.sh build/$(ARCH)-test-nmi-real/nmi-real.serial.log
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=modules iso
	scripts/exit-test.sh modules build/$(ARCH)-test-modules/vos.iso 33 \
	    'boot: kernel command line "' 'vos.test=1' 'boot: modules 2' 'boot: module 0 ' \
	    'boot: module 1 ' 'selftest: modules ' 'selftest: pmm-kernel ' 'selftest: vmm ' \
	    'selftest: user ' 'VOS-TEST: PASS' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=pmm-exhaust iso
	scripts/exit-test.sh pmm-exhaust build/$(ARCH)-test-pmm-exhaust/vos.iso 33 \
	    'selftest: drained ' 'selftest: pmm-exhaust ' 'VOS-TEST: PASS' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-lowaccess iso
	scripts/exit-test.sh kernel-lowaccess build/$(ARCH)-test-kernel-lowaccess/vos.iso 35 \
	    'vmm: identity probe 0x100000 = 0xe85250d6' 'vmm: identity map removed' \
	    'exc: #PF vec=14 err=0x0 cs=0x8 rip=' 'cr2=0x100000 ' 'panic: #PF page fault' \
	    'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=vmm-stale-tlb iso
	scripts/exit-test.sh vmm-stale-tlb build/$(ARCH)-test-vmm-stale-tlb/vos.iso 35 \
	    'vmm-test: stale-tlb read through VT ok' 'exc: #PF vec=14 err=0x0 cs=0x8 rip=' \
	    'cr2=0x200000000000 ' 'panic: #PF page fault' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=vmm-nx iso
	scripts/exit-test.sh vmm-nx build/$(ARCH)-test-vmm-nx/vos.iso 35 \
	    'vmm-test: kernel text control returned' \
	    'exc: #PF vec=14 err=0x11 cs=0x8 rip=0x200000000000 ' 'cr2=0x200000000000 ' \
	    'panic: #PF page fault' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=vmm-readonly iso
	scripts/exit-test.sh vmm-readonly build/$(ARCH)-test-vmm-readonly/vos.iso 35 \
	    'vmm-test: read-only page readable' 'exc: #PF vec=14 err=0x3 cs=0x8 rip=' \
	    'cr2=0x200000000000 ' 'panic: #PF page fault' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-int80 iso
	scripts/exit-test.sh kernel-int80 build/$(ARCH)-test-kernel-int80/vos.iso 35 \
	    'selftest: user ' 'exc: int $$0x80 from ring 0' 'panic: int $$0x80 from kernel mode' \
	    'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=kernel-irq-vector iso
	scripts/exit-test.sh kernel-irq-vector build/$(ARCH)-test-kernel-irq-vector/vos.iso 35 \
	    'selftest: irq ' 'exc: IRQ vector 0x20 in ring 0' \
	    'panic: hardware interrupt vector in ring 0 (the kernel runs with IF=0)' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=thread-overflow iso
	scripts/exit-test.sh thread-overflow build/$(ARCH)-test-thread-overflow/vos.iso 35 'selftest: sched ' \
	    'exc: #DF vec=8 err=0x0 cs=0x8' 'stack=ist1' 'panic: #DF double fault' 'VOS-TEST: FAIL' --
	scripts/check-fault.sh build/$(ARCH)-test-thread-overflow/thread-overflow.serial.log
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=sched-deadlock iso
	scripts/exit-test.sh sched-deadlock build/$(ARCH)-test-sched-deadlock/vos.iso 35 'selftest: sched ' \
	    'sched-test: the boot thread blocks on a queue nobody wakes' \
	    'panic: sched: every thread is blocked and none sleeps (lost wakeup?)' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=irq-top-sched iso
	scripts/exit-test.sh irq-top-sched build/$(ARCH)-test-irq-top-sched/vos.iso 35 'selftest: sched ' \
	    'sched-test: a top half calls thread_yield' 'panic: sched: called from an interrupt handler' \
	    'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=mp iso
	scripts/exit-test.sh mp build/$(ARCH)-test-mp/vos.iso 33 'mp: memtag1 + memtag2 + irqregs:' \
	    'mp: 3 x spin (fairness):' 'user[hello]: Hello from VOS!' 'selftest: mp ' ' cases, 0 failed' \
	    'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh mp-2G build/$(ARCH)-test-mp/vos.iso 33 'selftest: mp ' 'VOS-TEST: PASS' --
	scripts/exit-test.sh mp-max build/$(ARCH)-test-mp/vos.iso 33 'smep=on umip=on smap=on' 'selftest: mp ' \
	    'VOS-TEST: PASS' -- -cpu max
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=stress iso
	scripts/exit-test.sh stress build/$(ARCH)-test-stress/vos.iso 33 'stress: mixed load: ' \
	    'stress: fairness over ' 'selftest: stress ' ' cases, 0 failed' 'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh stress-2G build/$(ARCH)-test-stress/vos.iso 33 'selftest: stress ' 'VOS-TEST: PASS' --
	scripts/exit-test.sh stress-max build/$(ARCH)-test-stress/vos.iso 33 'smep=on umip=on smap=on' \
	    'selftest: stress ' 'VOS-TEST: PASS' -- -cpu max
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=ipc iso
	scripts/exit-test.sh ipc build/$(ARCH)-test-ipc/vos.iso 33 'selftest: ipc ' \
	    'ipc: s-pingpong a=ipc_ping b=ipc_echo order=b-blocked ea=exit:0 eb=exit:10 ia=10 ib=10 repeat=3 => PASS' \
	    'ipc: s-killwait ' 'ipc: s-v0compat ' 'ipc: 12 scenarios, 0 failed' 'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh ipc-2G build/$(ARCH)-test-ipc/vos.iso 33 'ipc: 12 scenarios, 0 failed' 'VOS-TEST: PASS' --
	scripts/exit-test.sh ipc-max build/$(ARCH)-test-ipc/vos.iso 33 'smep=on umip=on smap=on' \
	    'ipc: 12 scenarios, 0 failed' 'VOS-TEST: PASS' -- -cpu max
	@echo "ipc-forge: a verdict printed only by a program (ipc_forge) must NOT be found"
	! scripts/exit-test.sh ipc-forge build/$(ARCH)-test-ipc/vos.iso 33 'ipc: s-forged-verdict => PASS' -- >/dev/null 2>&1
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=irq iso
	scripts/exit-test.sh irq build/$(ARCH)-test-irq/vos.iso 33 'irq: 8259 master 0x20-0x27' \
	    'suite: t-spin expect=budget ticks=50 ok=0 => PASS' 'suite: t-irqregs expect=budget ticks=50 ok=0 => PASS' \
	    'suite: t-iflock expect=budget ticks=20 ok=0 => PASS' 'suite: t-kernelif expect=exit status=0 ok=1 => PASS' \
	    'suite: final hello exited 0, all frames reclaimed' 'suite: 9 cases, 0 failed, hello rerun 10 times' \
	    'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh irq-2G build/$(ARCH)-test-irq/vos.iso 33 \
	    'suite: final hello exited 0, all frames reclaimed' 'suite: 9 cases, 0 failed, hello rerun 10 times' \
	    'VOS-TEST: PASS' --
	scripts/exit-test.sh irq-max build/$(ARCH)-test-irq/vos.iso 33 'smep=on umip=on smap=on' \
	    'suite: final hello exited 0, all frames reclaimed' 'suite: 9 cases, 0 failed, hello rerun 10 times' \
	    'VOS-TEST: PASS' -- -cpu max
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=app-ambiguous iso
	scripts/exit-test.sh app-ambiguous build/$(ARCH)-test-app-ambiguous/vos.iso 35 \
	    'app: 2 modules named "hello"; ambiguous, running none' \
	    'boot: check failed: boot application' 'selftest: elf 1 cases, 0 failed' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=app-reject iso
	scripts/exit-test.sh app-reject build/$(ARCH)-test-app-reject/vos.iso 35 \
	    'elf: hello: rejected: PT_LOAD outside [0x400000, 0x40000000)' \
	    'boot: check failed: boot application' 'selftest: elf 1 cases, 0 failed' 'VOS-TEST: FAIL' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=suite iso
	scripts/exit-test.sh suite build/$(ARCH)-test-suite/vos.iso 33 \
	    'suite: CPU protections: smep=off umip=off smap=off' 'suite: final hello exited 0, all frames reclaimed' \
	    ' cases, 0 failed, hello rerun ' 'user[t-df]: df: copied forward' 'VOS-TEST: PASS' --
	MEM=2G scripts/exit-test.sh suite-2G build/$(ARCH)-test-suite/vos.iso 33 \
	    'suite: final hello exited 0, all frames reclaimed' ' cases, 0 failed, hello rerun ' 'VOS-TEST: PASS' --
	scripts/exit-test.sh suite-max build/$(ARCH)-test-suite/vos.iso 33 \
	    'suite: CPU protections: smep=on umip=on smap=on' 'suite: final hello exited 0, all frames reclaimed' \
	    ' cases, 0 failed, hello rerun ' 'VOS-TEST: PASS' -- -cpu max
	@echo "suite: the byte 0x01 written by t-outb must never reach the serial port"
	! LC_ALL=C grep -q "$$(printf '\001')" build/$(ARCH)-test-suite/suite.serial.log build/$(ARCH)-test-suite/suite-max.serial.log
	@echo "suite-forge: a suite verdict printed only by t-forge must NOT be found"
	! scripts/exit-test.sh suite-forge build/$(ARCH)-test-suite/vos.iso 33 'suite: t-nonexistent: PASS' -- >/dev/null 2>&1
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=smep-exec iso
	scripts/exit-test.sh smep-max build/$(ARCH)-test-smep-exec/vos.iso 35 \
	    'smep: kernel calling the user page at 0x400000' 'exc: #PF vec=14 err=0x11 cs=0x8 rip=0x400000 ' \
	    'cr2=0x400000 ' 'panic: #PF page fault' -- -cpu max
	scripts/exit-test.sh smep-qemu64 build/$(ARCH)-test-smep-exec/vos.iso 33 \
	    'smep: the kernel executed a user page (SMEP not enabled)' 'VOS-TEST: PASS' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=smap-touch iso
	scripts/exit-test.sh smap-max build/$(ARCH)-test-smap-touch/vos.iso 35 \
	    'smap: user sets AC, kernel reads the user page 0x400000' 'exc: #PF vec=14 err=0x1 cs=0x8' \
	    'cr2=0x400000 ' 'panic: #PF page fault' -- -cpu max
	scripts/exit-test.sh smap-qemu64 build/$(ARCH)-test-smap-touch/vos.iso 33 \
	    'smap: the kernel read a user page (SMAP not enabled)' 'VOS-TEST: PASS' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=vblk iso
	TIMEOUT=90 scripts/exit-test.sh vblk build/$(ARCH)-test-vblk/vos.iso 33 \
	    ' id 0x1042 (modern), ready, 2051 sectors, queue 16 of ' 'features VERSION_1 RO (offered ' \
	    'selftest: vblk ' 'selftest: blk ' 'vblk: device shut down, DMA pool released' \
	    'vblk: hung request beside a CPU-bound program: without yielding the program ran 0 of 0 ticks' \
	    ', interrupts on IRQ ' 'vblk: 64 reads in interrupt mode: ' \
	    'vblk: IRQ 11 is shared with the e1000 at 00:' \
	    'vblk: 300 reads of 8 sectors beside a CPU-bound program, interrupt mode: ' \
	    'vblk: 300 reads of 8 sectors beside a CPU-bound program, polling mode: ' \
	    'VOS-TEST: PASS' -- $(VBLK_RO)
	@echo "vblk-nonic: without QEMU's default NIC the interrupt line is not shared"
	TIMEOUT=90 scripts/exit-test.sh vblk-nonic build/$(ARCH)-test-vblk/vos.iso 33 \
	    ', interrupts on IRQ ' 'is not shared with another known device' 'selftest: vblk ' 'selftest: blk ' \
	    'VOS-TEST: PASS' -- -nic none $(VBLK_RO)
	@echo "vblk-slot5: the line comes from the device's PCI configuration (slot 5 routes to IRQ 10)"
	TIMEOUT=90 scripts/exit-test.sh vblk-slot5 build/$(ARCH)-test-vblk/vos.iso 33 \
	    'vblk: virtio-blk at 00:0x5.0 ' ', interrupts on IRQ 10 (INTA, level)' 'selftest: vblk ' 'selftest: blk ' \
	    'VOS-TEST: PASS' -- -drive file=$(VBLK_DISK),if=none,id=vd0,format=raw,readonly=on \
	    -device virtio-blk-pci,drive=vd0,disable-legacy=on,addr=0x5
	MEM=2G TIMEOUT=90 scripts/exit-test.sh vblk-2G build/$(ARCH)-test-vblk/vos.iso 33 \
	    ' (modern), ready, 2051 sectors' 'selftest: vblk ' 'selftest: blk ' 'VOS-TEST: PASS' -- $(VBLK_RO)
	TIMEOUT=90 scripts/exit-test.sh vblk-max build/$(ARCH)-test-vblk/vos.iso 33 \
	    'smep=on umip=on smap=on' ' (modern), ready, 2051 sectors' 'selftest: vblk ' 'selftest: blk ' 'VOS-TEST: PASS' \
	    -- $(VBLK_RO) -cpu max
	TIMEOUT=90 scripts/exit-test.sh vblk-transitional build/$(ARCH)-test-vblk/vos.iso 33 \
	    ' id 0x1001 (transitional, driven as modern), ready, 2051 sectors' 'features VERSION_1 RO (offered ' \
	    'selftest: vblk ' 'selftest: blk ' 'VOS-TEST: PASS' \
	    -- -drive file=$(VBLK_DISK),if=none,id=vd0,format=raw,readonly=on -device virtio-blk-pci,drive=vd0
	@echo "vblk-writable: a writable device (no VIRTIO_BLK_F_RO) is still only read; its image must be unchanged"
	rm -f $(VBLK_RW_COPY) && cp $(VBLK_DISK) $(VBLK_RW_COPY) && chmod u+w $(VBLK_RW_COPY)
	TIMEOUT=90 scripts/exit-test.sh vblk-writable build/$(ARCH)-test-vblk/vos.iso 33 \
	    ' (modern), ready, 2051 sectors' 'features VERSION_1 (offered ' 'selftest: vblk ' 'selftest: blk ' 'VOS-TEST: PASS' \
	    -- -drive file=$(VBLK_RW_COPY),if=none,id=vd0,format=raw -device virtio-blk-pci,drive=vd0,disable-legacy=on
	echo "$(VBLK_SHA256)  $(VBLK_RW_COPY)" | sha256sum -c
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=vblk-ioerr iso
	TIMEOUT=90 scripts/exit-test.sh vblk-ioerr build/$(ARCH)-test-vblk-ioerr/vos.iso 33 \
	    ' (modern), ready, 2051 sectors' 'selftest: vblk ' 'selftest: blk ' ' 54 I/O errors, ' 'VOS-TEST: PASS' \
	    -- -drive file=blkdebug:tests/x86_64/vblk-ioerr.blkdebug:$(VBLK_DISK),if=none,id=vd0,format=raw,readonly=on \
	    -device virtio-blk-pci,drive=vd0,disable-legacy=on
	@echo "vblk: devices the driver must refuse leave the kernel running without block storage"
	scripts/exit-test.sh vblk-iommu-platform $(TEST_ISO) 33 \
	    ' (modern), failed: device requires VIRTIO_F_ACCESS_PLATFORM (no IOMMU support)' \
	    'vblk: refused device: device requires VIRTIO_F_ACCESS_PLATFORM' 'selftest: vblk ' 'selftest: blk ' 'VOS-TEST: PASS' \
	    -- -drive file=$(VBLK_DISK),if=none,id=vd0,format=raw,readonly=on \
	    -device virtio-blk-pci,drive=vd0,disable-legacy=on,iommu_platform=on
	scripts/exit-test.sh vblk-queue8 $(TEST_ISO) 33 ' (modern), failed: queue too small' \
	    'vblk: refused device: queue too small' 'selftest: vblk ' 'selftest: blk ' 'VOS-TEST: PASS' \
	    -- -drive file=$(VBLK_DISK),if=none,id=vd0,format=raw,readonly=on \
	    -device virtio-blk-pci,drive=vd0,disable-legacy=on,queue-size=8
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=ext2 iso
	TIMEOUT=240 scripts/exit-test.sh ext2 build/$(ARCH)-test-ext2/vos.iso 33 \
	    'ext2: ext2-1k.img: ' 'ext2: ext2-2k.img: ' 'ext2: ext2-4k.img: ' 'ext2: ext2-htree.img: ' \
	    'ext2: ext2-nofeat.img: ' 'ext2: ext2-empty.img: ' 'ext2: virtio-blk: ' \
	    ' corrupted or failing images refused for the intended reason' 'selftest: ext2 ' 'VOS-TEST: PASS' \
	    -- $(EXT2_DISK)
	MEM=2G TIMEOUT=240 scripts/exit-test.sh ext2-2G build/$(ARCH)-test-ext2/vos.iso 33 \
	    'ext2: virtio-blk: ' 'selftest: ext2 ' 'VOS-TEST: PASS' -- $(EXT2_DISK)
	TIMEOUT=240 scripts/exit-test.sh ext2-max build/$(ARCH)-test-ext2/vos.iso 33 \
	    'smep=on umip=on smap=on' 'ext2: virtio-blk: ' 'selftest: ext2 ' 'VOS-TEST: PASS' -- $(EXT2_DISK) -cpu max
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=file iso
	TIMEOUT=240 scripts/exit-test.sh file build/$(ARCH)-test-file/vos.iso 33 \
	    'selftest: file 845 cases, 0 failed' 'VOS-TEST: PASS' -- $(EXT2_DISK)
	MEM=2G TIMEOUT=240 scripts/exit-test.sh file-2G build/$(ARCH)-test-file/vos.iso 33 \
	    'selftest: file 845 cases, 0 failed' 'VOS-TEST: PASS' -- $(EXT2_DISK)
	TIMEOUT=240 scripts/exit-test.sh file-max build/$(ARCH)-test-file/vos.iso 33 \
	    'smep=on umip=on smap=on' 'selftest: file 845 cases, 0 failed' 'VOS-TEST: PASS' -- $(EXT2_DISK) -cpu max
	@echo "ext2: the fixture images are unchanged after every run"
	cd $(EXT2_DIR) && sha256sum -c --quiet SHA256SUMS
	@echo "vblk: the test disk is unchanged after every run"
	echo "$(VBLK_SHA256)  $(VBLK_DISK)" | sha256sum -c
	@echo "U1: the early-boot font table is exactly the output of its generator"
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=scope-exit iso
	scripts/exit-test.sh scope-exit build/$(ARCH)-test-scope-exit/vos.iso 35 \
	    'panic: io_scope: thread exited without unwinding' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=scope-live-end iso
	scripts/exit-test.sh scope-live-end build/$(ARCH)-test-scope-live-end/vos.iso 35 \
	    'panic: io_scope: end with wrong owner or live request' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=scope-orphan-idle iso
	scripts/exit-test.sh scope-orphan-idle build/$(ARCH)-test-scope-orphan-idle/vos.iso 35 \
	    'scope: only orphan deadline remains; no sleeping thread' \
	    'vblk: device reset: an abandoned request never completed' \
	    'panic: sched: every thread is blocked and none sleeps (lost wakeup?)' -- $(VBLK_RO)
	python3 -I scripts/font8x16.py | cmp - $(ARCH_DIR)/font8x16.c
	@echo "U1: the BIOS screen console (GRUB hands over a VBE framebuffer): PASS badge, then a panic banner"
	COLOR=2ea043 scripts/halt-test.sh bios-screen $(ISO) $(KERNEL) 25 main 'screen: framebuffer ' \
	    'boot: checks passed' 'user[hello]: Hello from VOS!' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=panic iso
	COLOR=b62324 scripts/halt-test.sh bios-panic-screen build/$(ARCH)-test-panic/vos.iso \
	    build/$(ARCH)-test-panic/vos.bin 25 panic 'panic: test case: deliberate panic' --
	$(MAKE) --no-print-directory ARCH=$(ARCH) user-test

# ---- U1: UEFI (q35 + OVMF, Secure Boot not enabled, fresh variable store per run) ----
#
# Every image is the BIOS image's kernel and modules on a GRUB x86_64-efi ISO
# (make uefi-iso). Needs GRUB_EFI_DIR (grub-efi-amd64-bin) and mtools; see
# docs/plans/uefi.md. Images that halt are judged by their final CPU state
# (scripts/halt-test.sh), the test images by their exit status and markers.
UEFI_RUN := OVMF=1
define uefi-image
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 VOS_TEST_CASE=$(1) uefi-iso
endef
arch-test-uefi:
	$(MAKE) --no-print-directory ARCH=$(ARCH) uefi-iso
	$(MAKE) --no-print-directory ARCH=$(ARCH) UEFI_HEAD=tests/x86_64/uefi-nogop-head.cfg UEFI_TAG=-nogop uefi-iso
	@echo "uefi: the ISO has an EFI boot path: El Torito UEFI entry, EFI System Partition, BOOTX64.EFI"
	xorriso -indev build/$(ARCH)/vos-uefi.iso -report_el_torito plain 2>/dev/null | grep -q 'El Torito boot img :   1  UEFI'
	xorriso -indev build/$(ARCH)/vos-uefi.iso -report_system_area plain 2>/dev/null | \
	    grep -q 'GPT type GUID .*28732ac11ff8d211ba4b00a0c93ec93b'
	rm -f build/$(ARCH)/efi-check.img
	xorriso -osirrox on -indev build/$(ARCH)/vos-uefi.iso -extract /efi.img build/$(ARCH)/efi-check.img >/dev/null 2>&1
	mdir -i build/$(ARCH)/efi-check.img ::/efi/boot/bootx64.efi >/dev/null
	$(UEFI_RUN) COLOR=2ea043 scripts/halt-test.sh uefi-boot build/$(ARCH)/vos-uefi.iso $(KERNEL) 35 main \
	    'user[hello]: Hello from VOS!' 'boot: checks passed' 'screen: framebuffer 1280x800 BGRX at ' \
	    'cpu: inherited cr0=0x80010033' 'devmap: window at ' --
	$(UEFI_RUN) MEDIA=usb MEM=8G SPARSE_MEM=1 scripts/halt-test.sh uefi-boot-usb-8G build/$(ARCH)/vos-uefi.iso \
	    $(KERNEL) 45 main 'user[hello]: Hello from VOS!' 'boot: checks passed' 'bytes of RAM above 1 GiB ignored' --
	@echo "uefi-boot-noserial: no serial port at all; the screenshot must show the PASS badge"
	$(UEFI_RUN) SERIAL=0 COLOR=2ea043 scripts/halt-test.sh uefi-boot-noserial build/$(ARCH)/vos-uefi.iso $(KERNEL) 35 main --
	$(UEFI_RUN) scripts/halt-test.sh uefi-nofb build/$(ARCH)/vos-uefi-nogop.iso $(KERNEL) 35 main \
	    'screen: none, serial only: UEFI boot without framebuffer information' 'boot: checks passed' \
	    'user[hello]: Hello from VOS!' --
	@echo "uefi: other GOP modes (GRUB's Multiboot2 loader keeps the firmware's mode; OVMF takes the EDID-preferred one)"
	$(UEFI_RUN) COLOR=2ea043 scripts/halt-test.sh uefi-640x480 build/$(ARCH)/vos-uefi.iso $(KERNEL) 35 main \
	    'screen: framebuffer 640x480 BGRX' 'boot: checks passed' -- -vga none -device VGA,edid=on,xres=640,yres=480
	$(UEFI_RUN) COLOR=2ea043 scripts/halt-test.sh uefi-1920x1080 build/$(ARCH)/vos-uefi.iso $(KERNEL) 35 main \
	    'screen: framebuffer 1920x1080 BGRX' 'boot: checks passed' -- -vga none -device VGA,edid=on,xres=1920,yres=1080
	$(MAKE) --no-print-directory ARCH=$(ARCH) VOS_TEST=1 uefi-iso
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-test $(TEST_ISO:vos.iso=vos-uefi.iso) 33 \
	    'screen: framebuffer ' 'selftest: bootinfo ' 'selftest: kmap ' 'selftest: user ' 'selftest: sched ' \
	    'selftest: ipc ' 'selftest: screen ' 'NOT APPLICABLE: level-triggered registration' 'VOS-TEST: PASS' --
	$(UEFI_RUN) TIMEOUT=240 MEM=8G SPARSE_MEM=1 scripts/exit-test.sh uefi-test-8G $(TEST_ISO:vos.iso=vos-uefi.iso) 33 \
	    'bytes of RAM above 1 GiB ignored' 'selftest: pmm-kernel ' 'VOS-TEST: PASS' --
	$(call uefi-image,panic)
	$(UEFI_RUN) COLOR=b62324 scripts/halt-test.sh uefi-panic-screen build/$(ARCH)-test-panic/vos-uefi.iso \
	    build/$(ARCH)-test-panic/vos.bin 35 panic 'panic: test case: deliberate panic' --
	$(call uefi-image,suite)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-suite build/$(ARCH)-test-suite/vos-uefi.iso 33 \
	    'suite: final hello exited 0, all frames reclaimed' ' cases, 0 failed, hello rerun ' 'VOS-TEST: PASS' --
	$(call uefi-image,ipc)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-ipc build/$(ARCH)-test-ipc/vos-uefi.iso 33 \
	    'ipc: 12 scenarios, 0 failed' 'VOS-TEST: PASS' --
	$(call uefi-image,mp)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-mp build/$(ARCH)-test-mp/vos-uefi.iso 33 \
	    'mp: 3 x spin (fairness):' 'selftest: mp ' ' cases, 0 failed' 'VOS-TEST: PASS' --
	$(call uefi-image,stress)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-stress build/$(ARCH)-test-stress/vos-uefi.iso 33 \
	    'stress: blocked receiver: ' 'selftest: stress ' ' cases, 0 failed' 'VOS-TEST: PASS' --
	$(call uefi-image,irq)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-irq build/$(ARCH)-test-irq/vos-uefi.iso 33 \
	    'suite: t-spin expect=budget ticks=50 ok=0 => PASS' 'suite: 9 cases, 0 failed, hello rerun 10 times' 'VOS-TEST: PASS' --
	$(call uefi-image,vblk)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-vblk build/$(ARCH)-test-vblk/vos-uefi.iso 33 \
	    'polled (interrupt line not level triggered (ELCR))' 'NOT APPLICABLE: virtio-blk interrupt-mode tests' \
	    'vblk: 300 reads of 8 sectors beside a CPU-bound program, polling mode: ' 'selftest: vblk ' 'selftest: blk ' \
	    'VOS-TEST: PASS' -- $(VBLK_RO)
	$(call uefi-image,ext2)
	$(UEFI_RUN) TIMEOUT=240 scripts/exit-test.sh uefi-ext2 build/$(ARCH)-test-ext2/vos-uefi.iso 33 \
	    'ext2: virtio-blk: ' 'NOT APPLICABLE: ext2 readers interrupted while waiting for the device' \
	    'selftest: ext2 ' 'VOS-TEST: PASS' -- $(EXT2_DISK)
	$(UEFI_RUN) MEM=128M TIMEOUT=240 scripts/exit-test.sh uefi-ext2-max build/$(ARCH)-test-ext2/vos-uefi.iso 33 \
	    'ext2: virtio-blk: ' 'selftest: ext2 ' 'VOS-TEST: PASS' -- $(EXT2_DISK) -cpu max
	$(call uefi-image,file)
	$(UEFI_RUN) MEM=128M TIMEOUT=240 scripts/exit-test.sh uefi-file build/$(ARCH)-test-file/vos-uefi.iso 33 \
	    'selftest: file 845 cases, 0 failed' 'VOS-TEST: PASS' -- $(EXT2_DISK)
	$(UEFI_RUN) MEM=128M TIMEOUT=240 scripts/exit-test.sh uefi-file-max build/$(ARCH)-test-file/vos-uefi.iso 33 \
	    'selftest: file 845 cases, 0 failed' 'VOS-TEST: PASS' -- $(EXT2_DISK) -cpu max
	@echo "uefi: the fixture images and the test disk are unchanged after every run"
	cd $(EXT2_DIR) && sha256sum -c --quiet SHA256SUMS
	echo "$(VBLK_SHA256)  $(VBLK_DISK)" | sha256sum -c

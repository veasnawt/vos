# VOS user space (x86_64): the ABI v0 runtime and applications. Built and
# linked separately from the kernel: no kernel source, object or symbol is
# involved, and no host startup file or library is linked.
#
#   make ARCH=x86_64 user        build build/x86_64/user/<app>.elf
#   make ARCH=x86_64 user-check  validate every application (scripts/check-user-elf.sh)
#   make ARCH=x86_64 user-test   validation plus negative tests (scripts/test-user-elf.sh)
# Nothing here is put on the boot ISO or executed (that is N1.8).

USER_DIR   := user
USER_BUILD := build/$(ARCH)/user
USER_APPS  := hello
USER_LDS   := $(USER_DIR)/lib/user.ld
USER_RT    := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o abi_note.o)
# ABI v1 executables (N2.4): the v1 note and the v1 stubs instead of the v0 note.
USER_RT_V1 := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o syscall_v1.o abi_note_v1.o)
USER_RT_V2 := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o syscall_v1.o syscall_v2.o abi_note_v2.o)
USER_RT_V3 := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o syscall_v1.o syscall_v2.o syscall_v3.o abi_note_v3.o)
USER_RT_V4 := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o syscall_v1.o syscall_v2.o syscall_v3.o syscall_v4.o abi_note_v4.o)
FILE_DIR := $(USER_BUILD)/filetests
FILE_ELFS := $(addprefix $(FILE_DIR)/,probe.elf reader.elf reader_y.elf error.elf gate0.elf gate1.elf boundary.elf nogrant.elf cookie.elf regs.elf adversary.elf)
# Separate Gate A companion; no change to the frozen file/suite lists.
STORAGE_STRESS_DIR := $(USER_BUILD)/storage_stress
$(STORAGE_STRESS_DIR)/canary.elf: $(STORAGE_STRESS_DIR)/canary.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
.PHONY: user-files
user-files: $(FILE_ELFS)
$(FILE_DIR)/%.elf: $(USER_BUILD)/filetests/%.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
$(FILE_DIR)/reader.elf: $(USER_BUILD)/filetests/reader.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
$(FILE_DIR)/reader_y.o: $(USER_DIR)/filetests/reader.c Makefile $(USER_DIR)/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DEXPECT_Y -MMD -MP -c $< -o $@
$(FILE_DIR)/error.o: $(USER_DIR)/filetests/reader.c Makefile $(USER_DIR)/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DEXPECT_ERROR -MMD -MP -c $< -o $@
$(FILE_DIR)/error.elf: $(FILE_DIR)/error.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
$(FILE_DIR)/gate0.o $(FILE_DIR)/gate1.o: $(USER_DIR)/filetests/gate.c Makefile $(USER_DIR)/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DTEST_ABI=$(if $(findstring gate1,$@),1,0) -MMD -MP -c $< -o $@
$(FILE_DIR)/gate0.elf: $(FILE_DIR)/gate0.o $(USER_RT) $(USER_BUILD)/lib/syscall_v1.o $(USER_BUILD)/lib/syscall_v2.o $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT) $(USER_BUILD)/lib/syscall_v1.o $(USER_BUILD)/lib/syscall_v2.o $<
$(FILE_DIR)/gate1.elf: $(FILE_DIR)/gate1.o $(USER_RT_V1) $(USER_BUILD)/lib/syscall_v2.o $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V1) $(USER_BUILD)/lib/syscall_v2.o $<

# Freestanding, static, non-PIC, general registers only (no x87/MMX/SSE),
# no stack protector (it needs TLS), no CET notes, no unwind tables. Only the
# compiler's own freestanding headers and user/include are visible. The red
# zone stays enabled: the kernel never writes to the user stack.
USER_CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -m64 -mcmodel=small \
                -ffreestanding -fno-builtin -nostdinc \
                -isystem $(shell $(CC) -print-file-name=include) -I$(USER_DIR)/include \
                -fno-pic -fno-pie -fno-stack-protector -fno-stack-clash-protection \
                -fcf-protection=none -fno-asynchronous-unwind-tables -fno-unwind-tables \
                -mgeneral-regs-only
USER_ASFLAGS := -m64 -I$(USER_DIR)/include
USER_LDFLAGS_BASE := -m elf_x86_64 -nostdlib -static -z noexecstack -z max-page-size=0x1000 \
                     -z norelro --build-id=none
USER_LDFLAGS := $(USER_LDFLAGS_BASE) -T $(USER_LDS)

USER_ELFS := $(addprefix $(USER_BUILD)/,$(addsuffix .elf,$(USER_APPS)))

.PHONY: user user-check user-test

user: $(USER_RT) $(USER_ELFS)

$(USER_BUILD)/%.o: $(USER_DIR)/%.c Makefile $(USER_DIR)/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD)/%.o: $(USER_DIR)/%.S Makefile $(USER_DIR)/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_ASFLAGS) -MMD -MP -c $< -o $@

# <app>.elf from user/<app>/*.c, with crt0 first.
define USER_APP
$(USER_BUILD)/$(1).elf: $(USER_RT) $(patsubst $(USER_DIR)/%.c,$(USER_BUILD)/%.o,$(wildcard $(USER_DIR)/$(1)/*.c)) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $$@ $(USER_RT) $(patsubst $(USER_DIR)/%.c,$(USER_BUILD)/%.o,$(wildcard $(USER_DIR)/$(1)/*.c))
endef
$(foreach a,$(USER_APPS),$(eval $(call USER_APP,$(a))))

# N1.9 isolation-suite fixtures (user/suite): real VOS executables built like
# applications. regs has its own entry point (no crt0) to see the raw entry
# state; irqregs (N2.1) is assembly called as main by crt0. bad-<kind>.elf are malformed copies of hello.elf (scripts/mkbadelf.sh).
SUITE_DIR    := $(USER_BUILD)/suite
SUITE_C      := $(basename $(notdir $(wildcard $(USER_DIR)/suite/*.c)))
SUITE_OWN    := regs
SUITE_S      := irqregs ipc_regs
SUITE_BAD    := magic dyn wx entry abi interp trunc overlap
SUITE_ELFS   := $(addprefix $(SUITE_DIR)/,$(addsuffix .elf,$(SUITE_C) $(SUITE_S) $(SUITE_OWN))) \
                $(addprefix $(SUITE_DIR)/bad-,$(addsuffix .elf,$(SUITE_BAD)))

$(SUITE_DIR)/%.elf: $(USER_BUILD)/suite/%.o $(USER_RT) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT) $<

# ipc_*.c / ipc_*.S are ABI v1 programs (N2.4).
$(SUITE_DIR)/ipc_%.elf: $(USER_BUILD)/suite/ipc_%.o $(USER_RT_V1) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V1) $<

$(SUITE_DIR)/regs.elf: $(USER_BUILD)/suite/regs.o $(USER_BUILD)/lib/abi_note.o $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_BUILD)/lib/abi_note.o $<

$(SUITE_DIR)/bad-%.elf: $(USER_BUILD)/hello.elf scripts/mkbadelf.sh
	@mkdir -p $(dir $@)
	scripts/mkbadelf.sh $< $@ $*

.PHONY: user-suite
user-suite: $(SUITE_ELFS)

user-check: $(USER_ELFS)
	@set -e; for f in $(USER_ELFS); do scripts/check-user-elf.sh $$f; done

# Same ELF validator and Rust archive as the kernel. Non-PIE host linking is
# required by the archive's kernel/static relocations (including the panic bridge).
# Standalone C-only test builds remain available through elf.c's hosted reference.
ELFCHECK := build/$(ARCH)/tools/elfcheck
$(ELFCHECK): scripts/elfcheck.c arch/x86_64/elf.c arch/x86_64/elf.h arch/x86_64/rust_kernel.h $(LIBS) user/user.mk
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -DVOS_ELF_USE_RUST -fno-pie -no-pie -Iarch/x86_64 -o $@ scripts/elfcheck.c arch/x86_64/elf.c $(LIBS)

# Validation, profile and separation checks, and negative tests. Static only:
# nothing is loaded or run. The kernel images are checked not to contain the
# application.
user-test: user-check user-files $(USER_RT) $(USER_RT_V1) $(KERNEL) $(ELFCHECK) $(SUITE_ELFS)
	SUITE_DIR=$(SUITE_DIR) ELFCHECK=$(ELFCHECK) USER_CC="$(CC)" USER_CFLAGS="$(USER_CFLAGS)" USER_LD="$(LD)" \
	USER_LDFLAGS="$(USER_LDFLAGS_BASE)" USER_LDS=$(USER_LDS) \
	KERNEL_SYSCALL_H=arch/x86_64/syscall.h USER_SYSCALL_H=$(USER_DIR)/include/vos/syscall.h \
	    scripts/test-user-elf.sh $(USER_BUILD) $(KERNEL) $(wildcard build/$(ARCH)-test*/vos.bin)
	@set -e; for f in $(FILE_ELFS); do scripts/check-user-elf.sh $$f; $(ELFCHECK) $$f; done
	python3 -I scripts/test-file-abi.py

-include $(shell find $(USER_BUILD) -name '*.d' 2>/dev/null)

# Rust is compiled as a freestanding static library, linked through the same C ABI,
# entry point, ELF profile and syscall stubs as other native languages.
RUSTC ?= rustc
RUST_APP ?= rust-demo
VOS_RUST_FLAGS := --edition=2024 --target=x86_64-unknown-none -C panic=abort -C opt-level=s -C relocation-model=static -C code-model=small -C debuginfo=0
RUST_SDK := $(USER_BUILD)/rust/libvos.rlib
$(RUST_SDK): user/rust/vos.rs user/user.mk
	@mkdir -p $(dir $@)
	$(RUSTC) $(VOS_RUST_FLAGS) --crate-name vos --crate-type=rlib --emit=link,dep-info=$(@:.rlib=.d) -o $@ $<
define RUST_APPLICATION
$(USER_BUILD)/$(1)/main.a: user/$(1)/main.rs $(RUST_SDK) $(if $(filter vsh,$(1)),$(USER_BUILD)/rust/libvos_apps.rlib) user/user.mk
	@mkdir -p $$(dir $$@)
	$$(RUSTC) $$(VOS_RUST_FLAGS) --crate-name $$(subst -,_,$(1)) --crate-type=staticlib --extern vos=$$(RUST_SDK) $(if $(filter vsh,$(1)),-L dependency=$(USER_BUILD)/rust --extern vos_apps=$(USER_BUILD)/rust/libvos_apps.rlib) --emit=link,dep-info=$$(@:.a=.d) -o $$@ $$<
$(USER_BUILD)/$(1).elf: $(if $(filter vsh,$(1)),$(USER_RT_V4),$(USER_RT_V3)) $(USER_BUILD)/$(1)/main.a $(USER_BUILD)/rust/runtime.o $(USER_LDS)
	$$(LD) $$(USER_LDFLAGS) --gc-sections -o $$@ $(if $(filter vsh,$(1)),$(USER_RT_V4),$(USER_RT_V3)) $(USER_BUILD)/rust/runtime.o $(USER_BUILD)/$(1)/main.a
endef
$(foreach app,$(sort vsh $(RUST_APP)),$(eval $(call RUST_APPLICATION,$(app))))
.PHONY: user-vsh user-rust
user-vsh: $(USER_BUILD)/vsh.elf
user-rust: $(USER_BUILD)/$(RUST_APP).elf

define CONSOLE_PROBE
$(USER_BUILD)/console-$(1).o: user/vsh/console_probe.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_CFLAGS) $(3) -c $$< -o $$@
$(USER_BUILD)/console-$(1).elf: $(USER_BUILD)/console-$(1).o $(USER_BUILD)/lib/crt0.o $(USER_BUILD)/lib/syscall.o $(USER_BUILD)/lib/syscall_v3.o $(USER_BUILD)/lib/$(2).o $(USER_LDS)
	$$(LD) $$(USER_LDFLAGS) -o $$@ $(USER_BUILD)/lib/crt0.o $(USER_BUILD)/lib/syscall.o $(USER_BUILD)/lib/syscall_v3.o $(USER_BUILD)/lib/$(2).o $$<
endef
$(eval $(call CONSOLE_PROBE,v0,abi_note,-DEXPECT_GATE=1))
$(eval $(call CONSOLE_PROBE,v1,abi_note_v1,-DEXPECT_GATE=1))
$(eval $(call CONSOLE_PROBE,v2,abi_note_v2,-DEXPECT_GATE=1))
$(eval $(call CONSOLE_PROBE,denied,abi_note_v3,-DEXPECT_GATE=9))
$(eval $(call CONSOLE_PROBE,owner,abi_note_v3,))


# Separate Gate B modules; existing application/file/accounting lists stay frozen.
$(STORAGE_STRESS_DIR)/r0.o $(STORAGE_STRESS_DIR)/r1.o: user/storage_stress/readers.c Makefile user/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DREADER_ROLE=$(if $(findstring r0.o,$@),0,1) -MMD -MP -c $< -o $@
$(STORAGE_STRESS_DIR)/r0.elf: $(STORAGE_STRESS_DIR)/r0.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
$(STORAGE_STRESS_DIR)/r1.elf: $(STORAGE_STRESS_DIR)/r1.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
$(STORAGE_STRESS_DIR)/lifecycle.elf: $(STORAGE_STRESS_DIR)/lifecycle.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<
$(STORAGE_STRESS_DIR)/cpu.o: user/storage_stress/companion.c Makefile user/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DCOMPANION_CPU -DCPU_EXPECTED=1717253697U -MMD -MP -c $< -o $@
$(STORAGE_STRESS_DIR)/cpu.elf: $(STORAGE_STRESS_DIR)/cpu.o $(USER_RT) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT) $<
$(STORAGE_STRESS_DIR)/ipc0.o $(STORAGE_STRESS_DIR)/ipc1.o: user/storage_stress/companion.c Makefile user/user.mk
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(if $(findstring ipc0.o,$@),-DCOMPANION_INITIATOR,) -MMD -MP -c $< -o $@
$(STORAGE_STRESS_DIR)/ipc0.elf: $(STORAGE_STRESS_DIR)/ipc0.o $(USER_RT_V1) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V1) $<
$(STORAGE_STRESS_DIR)/ipc1.elf: $(STORAGE_STRESS_DIR)/ipc1.o $(USER_RT_V1) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V1) $<

# Separate Gate C module; Gate A/B modules and the frozen lists are unchanged.
$(STORAGE_STRESS_DIR)/gatec.elf: $(STORAGE_STRESS_DIR)/gatec.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<

# Separate Gate D module; Gate A/B/C modules and the frozen lists are unchanged.
$(STORAGE_STRESS_DIR)/gated.elf: $(STORAGE_STRESS_DIR)/gated.o $(USER_RT_V2) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RT_V2) $<


# ABI v4 is opt-in; existing ABI v0-v3 application/runtime rules remain unchanged.
$(USER_BUILD)/rust/libvos_apps.rlib: user/rust/apps.rs $(RUST_SDK)
	@mkdir -p $(dir $@)
	$(RUSTC) $(VOS_RUST_FLAGS) --crate-name vos_apps --crate-type=rlib --extern vos=$(RUST_SDK) --emit=link,dep-info=$(@:.rlib=.d) -o $@ $<
$(USER_BUILD)/exec-probe/main.a: user/exec-probe/main.rs $(RUST_SDK) $(USER_BUILD)/rust/libvos_apps.rlib user/user.mk
	@mkdir -p $(dir $@)
	$(RUSTC) $(VOS_RUST_FLAGS) --crate-name exec_probe --crate-type=staticlib -L dependency=$(USER_BUILD)/rust --extern vos=$(RUST_SDK) --extern vos_apps=$(USER_BUILD)/rust/libvos_apps.rlib --emit=link,dep-info=$(@:.a=.d) -o $@ $<
$(USER_BUILD)/exec-probe.elf: $(USER_BUILD)/exec-probe/main.a $(USER_RT_V4) $(USER_BUILD)/exec-probe/control.o $(USER_BUILD)/rust/runtime.o $(USER_LDS)
	$(LD) $(USER_LDFLAGS) --gc-sections -o $@ $(USER_RT_V4) $(USER_BUILD)/exec-probe/control.o $(USER_BUILD)/rust/runtime.o $(USER_BUILD)/exec-probe/main.a

define EXEC_FIXTURE
$(USER_BUILD)/exec-$(1).o: user/exec-probe/child.c user/user.mk
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_CFLAGS) -DEXEC_CASE=$(2) -MMD -MP -c $$< -o $$@
$(USER_BUILD)/exec-$(1).elf: $(USER_BUILD)/exec-$(1).o $(sort $(3) $(USER_BUILD)/lib/syscall_v4.o) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) -o $$@ $(filter-out $(USER_BUILD)/lib/syscall_v4.o,$(3)) $(USER_BUILD)/lib/syscall_v4.o $(USER_BUILD)/exec-$(1).o
endef
$(eval $(call EXEC_FIXTURE,negative,0,$(USER_RT_V4)))
$(eval $(call EXEC_FIXTURE,fault,1,$(USER_RT_V4)))
$(eval $(call EXEC_FIXTURE,spin,2,$(USER_RT_V4)))
$(eval $(call EXEC_FIXTURE,recursive,3,$(USER_RT_V4)))
$(eval $(call EXEC_FIXTURE,contender,4,$(USER_RT_V4)))
$(eval $(call EXEC_FIXTURE,legacy0,5,$(USER_RT)))
$(eval $(call EXEC_FIXTURE,legacy1,5,$(USER_RT_V1)))
$(eval $(call EXEC_FIXTURE,legacy2,5,$(USER_RT_V2)))
$(eval $(call EXEC_FIXTURE,legacy3,5,$(USER_RT_V3)))
EXEC_FIXTURE_NAMES := negative fault spin recursive contender legacy0 legacy1 legacy2 legacy3
EXEC_FIXTURE_ELFS = $(addprefix $(USER_BUILD)/exec-,$(addsuffix .elf,$(EXEC_FIXTURE_NAMES)))
.PHONY: user-exec-probe
user-exec-probe: $(USER_BUILD)/exec-probe.elf $(EXEC_FIXTURE_ELFS)


# Native ABI v4 foreground-ownership probe; existing SDK/runtime conventions.
$(USER_BUILD)/handoff-probe/main.a: user/handoff-probe/main.rs $(RUST_SDK) $(USER_BUILD)/rust/libvos_apps.rlib user/user.mk
	@mkdir -p $(dir $@)
	$(RUSTC) $(VOS_RUST_FLAGS) --crate-name handoff_probe --crate-type=staticlib -L dependency=$(USER_BUILD)/rust --extern vos=$(RUST_SDK) --extern vos_apps=$(USER_BUILD)/rust/libvos_apps.rlib --emit=link,dep-info=$(@:.a=.d) -o $@ $<
$(USER_BUILD)/handoff-probe.elf: $(USER_BUILD)/handoff-probe/main.a $(USER_RT_V4) $(USER_BUILD)/handoff-probe/control.o $(USER_BUILD)/rust/runtime.o $(USER_LDS)
	$(LD) $(USER_LDFLAGS) --gc-sections -o $@ $(USER_RT_V4) $(USER_BUILD)/handoff-probe/control.o $(USER_BUILD)/rust/runtime.o $(USER_BUILD)/handoff-probe/main.a
.PHONY: user-handoff-probe
user-handoff-probe: $(USER_BUILD)/handoff-probe.elf


# Additive ABI v5 GUI SDK; existing application targets/runtime conventions remain.
USER_RT_V5 := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o syscall_v1.o syscall_v2.o syscall_v3.o syscall_v4.o syscall_v5.o abi_note_v5.o)
$(USER_BUILD)/rust/libvos_gui.rlib: user/rust/gui.rs $(RUST_SDK) user/user.mk
	@mkdir -p $(dir $@)
	$(RUSTC) $(VOS_RUST_FLAGS) --crate-name vos_gui --crate-type=rlib -L dependency=$(USER_BUILD)/rust --extern vos=$(RUST_SDK) --emit=link,dep-info=$(@:.rlib=.d) -o $@ $<
define GUI_APPLICATION
$(USER_BUILD)/$(1)/main.a: user/$(1)/main.rs $(RUST_SDK) $(USER_BUILD)/rust/libvos_gui.rlib user/user.mk
	@mkdir -p $$(dir $$@)
	$$(RUSTC) $$(VOS_RUST_FLAGS) --crate-name $$(subst -,_,$(1)) --crate-type=staticlib -L dependency=$(USER_BUILD)/rust --extern vos=$$(RUST_SDK) --extern vos_gui=$(USER_BUILD)/rust/libvos_gui.rlib --emit=link,dep-info=$$(@:.a=.d) -o $$@ $$<
$(USER_BUILD)/$(1).elf: $(USER_RT_V5) $(USER_BUILD)/$(1)/main.a $(USER_BUILD)/rust/runtime.o $(USER_LDS) $(if $(filter gui-probe,$(1)),$(USER_BUILD)/gui-probe/control.o)
	$$(LD) $$(USER_LDFLAGS) --gc-sections -o $$@ $$(USER_RT_V5) $(USER_BUILD)/rust/runtime.o $(USER_BUILD)/$(1)/main.a $(if $(filter gui-probe,$(1)),$(USER_BUILD)/gui-probe/control.o)
endef
$(foreach app,gui-demo gui-probe,$(eval $(call GUI_APPLICATION,$(app))))
.PHONY: user-gui
user-gui: $(USER_BUILD)/gui-demo.elf $(USER_BUILD)/gui-probe.elf
# Verification query is linked only into the dedicated probe, never the SDK/demo.
$(USER_BUILD)/gui-probe/control.o: user/lib/syscall_v5.S
	@mkdir -p $(dir $@)
	printf '.section .text\n.global vos_gui_verify\nvos_gui_verify:\n mov $$0x7e66,%%eax\n int $$0x80\n ret\n.section .note.GNU-stack,"",@progbits\n' | $(CC) $(USER_ASFLAGS) -x assembler -c -o $@ -


# N6.3 only: let rustc resolve shared target rlibs while producing native ELF.
# The pre-existing staticlib recipes above remain unchanged. No runtime copy,
# host CRT, dynamic library, new entry convention or userspace authority.
POINTER_NATIVE_RT := $(USER_BUILD)/rust/libvos_gui_native.a
$(POINTER_NATIVE_RT): $(USER_RT_V5) $(USER_BUILD)/rust/runtime.o
	@mkdir -p $(dir $@)
	$(AR) crsD $@ $^
define POINTER_APPLICATION
$(USER_BUILD)/$(1).elf: user/$(1)/main.rs $(RUST_SDK) $(USER_BUILD)/rust/libvos_gui.rlib $(POINTER_NATIVE_RT) $(USER_LDS) user/user.mk $(if $(filter pointer-probe,$(1)),$(USER_BUILD)/gui-probe/control.o)
	$$(RUSTC) $$(VOS_RUST_FLAGS) --crate-name $$(subst -,_,$(1)) --crate-type=bin -L dependency=$(USER_BUILD)/rust --extern vos=$$(RUST_SDK) --extern vos_gui=$(USER_BUILD)/rust/libvos_gui.rlib -L native=$(USER_BUILD)/rust -l static:+whole-archive=vos_gui_native -C linker=$$(LD) -C linker-flavor=ld -C default-linker-libraries=no -C link-arg=--gc-sections $$(foreach flag,$$(USER_LDFLAGS),-C link-arg=$$(flag)) $(if $(filter pointer-probe,$(1)),-C link-arg=$(USER_BUILD)/gui-probe/control.o) -C link-arg=-Map=$$(@:.elf=.map) --emit=link,dep-info=$$(@:.elf=.d) -o $$@ $$<
endef
$(foreach app,pointer-demo pointer-probe,$(eval $(call POINTER_APPLICATION,$(app))))
.PHONY: user-pointer
user-pointer: $(USER_BUILD)/pointer-demo.elf $(USER_BUILD)/pointer-probe.elf

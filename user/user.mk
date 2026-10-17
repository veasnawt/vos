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
SUITE_S      := irqregs
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

# The kernel's ELF validator (arch/x86_64/elf.c, unchanged) built for the host,
# so the tests can compare it with scripts/check-user-elf.sh file by file.
ELFCHECK := build/$(ARCH)/tools/elfcheck
$(ELFCHECK): scripts/elfcheck.c arch/x86_64/elf.c arch/x86_64/elf.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Iarch/x86_64 -o $@ scripts/elfcheck.c arch/x86_64/elf.c

# Validation, profile and separation checks, and negative tests. Static only:
# nothing is loaded or run. The kernel images are checked not to contain the
# application.
user-test: user-check $(USER_RT) $(USER_RT_V1) $(KERNEL) $(ELFCHECK) $(SUITE_ELFS)
	SUITE_DIR=$(SUITE_DIR) ELFCHECK=$(ELFCHECK) USER_CC="$(CC)" USER_CFLAGS="$(USER_CFLAGS)" USER_LD="$(LD)" \
	USER_LDFLAGS="$(USER_LDFLAGS_BASE)" USER_LDS=$(USER_LDS) \
	KERNEL_SYSCALL_H=arch/x86_64/syscall.h USER_SYSCALL_H=$(USER_DIR)/include/vos/syscall.h \
	    scripts/test-user-elf.sh $(USER_BUILD) $(KERNEL) $(wildcard build/$(ARCH)-test*/vos.bin)

-include $(shell find $(USER_BUILD) -name '*.d' 2>/dev/null)

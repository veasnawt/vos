# VOS user space (x86_64): the ABI v0 runtime and applications. Built and
# linked separately from the kernel: no kernel source, object or symbol is
# involved, and no host startup file or library is linked.
#
#   make ARCH=x86_64 user        build build/x86_64/user/<app>.elf
#   make ARCH=x86_64 user-check  validate every application (scripts/check-user-elf.sh)
#   make ARCH=x86_64 user-test   validation plus negative tests (scripts/test-user-elf.sh)

USER_DIR   := user
USER_BUILD := build/$(ARCH)/user
USER_APPS  := hello
USER_LDS   := $(USER_DIR)/lib/user.ld
USER_RT    := $(addprefix $(USER_BUILD)/lib/,crt0.o syscall.o abi_note.o)

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
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z noexecstack -z max-page-size=0x1000 \
                -z norelro --build-id=none -T $(USER_LDS)

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

user-check: $(USER_ELFS)
	@set -e; for f in $(USER_ELFS); do scripts/check-user-elf.sh $$f; done

-include $(shell find $(USER_BUILD) -name '*.d' 2>/dev/null)

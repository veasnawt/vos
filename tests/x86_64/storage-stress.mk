# Gate A only. The fixture MUST be explicitly supplied and independently validated.
GRUB_CFG := tests/x86_64/storage-stress.grub.cfg
STRESS_FIXTURE ?= build/storage-stress/stress.img
ISO_FILES = $(STRESS_FIXTURE):boot/storage-stress.img \
            $(STORAGE_STRESS_DIR)/canary.elf:boot/storage-canary.elf
$(STRESS_FIXTURE):
	@echo "Missing validated private fixture: $@ (see N3.6 Gate A plan)" >&2; exit 1

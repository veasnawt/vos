# Image fragment for VOS_TEST_CASE=app-reject (included by the top-level Makefile).
# The module is the kernel's own ELF without its debug sections (N3.1: with
# them, the test kernel file grew past the loader's 1 MiB size limit, which
# would be rejected before the PT_LOAD range check this case is about). The
# program headers and loadable contents are those of vos.bin.
GRUB_CFG  := tests/x86_64/app-reject.grub.cfg
ISO_FILES  = $(BUILD)/kernel-as-app.elf:boot/kernel-as-app.elf
$(BUILD)/kernel-as-app.elf: $(BUILD)/vos.bin
	objcopy --strip-debug $< $@

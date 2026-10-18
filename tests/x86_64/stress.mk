# Image fragment for VOS_TEST_CASE=stress (included by the top-level Makefile).
GRUB_CFG  := tests/x86_64/stress.grub.cfg
ISO_FILES  = $(foreach f,$(SUITE_ELFS),$(f):boot/suite/$(notdir $(f)))

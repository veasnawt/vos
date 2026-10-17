# Image fragment for VOS_TEST_CASE=suite (included by the top-level Makefile):
# every isolation-suite fixture (user/suite, built by user/user.mk) on the ISO.
GRUB_CFG  := tests/x86_64/suite.grub.cfg
ISO_FILES  = $(foreach f,$(SUITE_ELFS),$(f):boot/suite/$(notdir $(f)))

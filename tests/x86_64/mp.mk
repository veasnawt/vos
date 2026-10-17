# Image fragment for VOS_TEST_CASE=mp (included by the top-level Makefile):
# the N2.3 multi-process programs, built from user/suite like the isolation suite.
GRUB_CFG  := tests/x86_64/mp.grub.cfg
ISO_FILES  = $(foreach f,$(SUITE_ELFS),$(f):boot/suite/$(notdir $(f)))

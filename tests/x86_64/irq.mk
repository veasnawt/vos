# Image fragment for VOS_TEST_CASE=irq (included by the top-level Makefile):
# the N2.1 timer-interrupt cases, built from user/suite like the isolation suite.
GRUB_CFG  := tests/x86_64/irq.grub.cfg
ISO_FILES  = $(foreach f,$(SUITE_ELFS),$(f):boot/suite/$(notdir $(f)))

# Image fragment for VOS_TEST_CASE=ipc (included by the top-level Makefile): the
# N2.4 programs from user/suite and the carrier file of the scenario modules.
GRUB_CFG  := tests/x86_64/ipc.grub.cfg
ISO_FILES  = $(foreach f,$(SUITE_ELFS),$(f):boot/suite/$(notdir $(f))) \
             tests/fixtures/ipc-scenario.txt:boot/scenario.txt

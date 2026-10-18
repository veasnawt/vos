# Image fragment for VOS_TEST_CASE=vblk (N3.1, included by the top-level Makefile):
# the virtio-blk test disk doubles as the GRUB module "vblk-fixture", the
# independent copy every sector read through the driver is compared with;
# three suite programs run beside the reads (selftest_vblk.c).
GRUB_CFG  := tests/x86_64/vblk.grub.cfg
ISO_FILES  = $(VBLK_DISK):boot/vblk-fixture.img \
             $(foreach p,memtag1 memtag2 irqregs,$(SUITE_DIR)/$(p).elf:boot/suite/$(p).elf)

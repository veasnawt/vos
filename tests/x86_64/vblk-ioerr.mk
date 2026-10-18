# Image fragment for VOS_TEST_CASE=vblk-ioerr (N3.1): the test disk behind
# QEMU's blkdebug driver (tests/x86_64/vblk-ioerr.blkdebug), which fails every
# read covering sector 1000; the disk is also the GRUB module "vblk-fixture".
GRUB_CFG  := tests/x86_64/vblk-ioerr.grub.cfg
ISO_FILES  = $(VBLK_DISK):boot/vblk-fixture.img

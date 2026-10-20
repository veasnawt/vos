# Image fragment for VOS_TEST_CASE=ext2 (N3.3, included by the top-level Makefile):
# the ext2 fixture images and their manifests as GRUB modules (selftest_ext2.c),
# and three suite programs that run beside the reads. The 1 KiB image is also
# attached through virtio-blk (arch.mk).
GRUB_CFG  := tests/x86_64/ext2.grub.cfg
ISO_FILES  = $(foreach i,$(EXT2_IMAGES),$(EXT2_DIR)/ext2-$(i).img:boot/ext2/ext2-$(i).img \
                                        $(EXT2_DIR)/ext2-$(i).manifest:boot/ext2/ext2-$(i).manifest) \
             $(foreach p,memtag1 memtag2 irqregs,$(SUITE_DIR)/$(p).elf:boot/suite/$(p).elf)
ISO_FILES += $(foreach p,probe reader reader_y error gate0 gate1 boundary nogrant cookie regs adversary,$(FILE_DIR)/$(p).elf:boot/filetests/$(p).elf)

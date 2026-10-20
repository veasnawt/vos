# File API tests consume only ext2-1k.img. The separate ext2 image retains
# all six reader fixtures/manifests and concurrent suite programs. Keeping
# both suites in the runners avoids GRUB's fragmented 128 MiB combined payload.
GRUB_CFG := tests/x86_64/file.grub.cfg
ISO_FILES = $(EXT2_DIR)/ext2-1k.img:boot/ext2/ext2-1k.img \
            $(foreach f,$(FILE_ELFS),$(f):boot/filetests/$(notdir $(f)))

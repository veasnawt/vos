# N3.6 I2 only; existing fixture and unchanged reader/error ELF modules.
GRUB_CFG := tests/x86_64/accounting.grub.cfg
ISO_FILES = $(EXT2_DIR)/ext2-1k.img:boot/ext2/ext2-1k.img \
            $(FILE_DIR)/reader.elf:boot/filetests/reader.elf \
            $(FILE_DIR)/error.elf:boot/filetests/error.elf

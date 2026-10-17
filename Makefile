CC      := gcc
LD      := ld
CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-builtin -fno-asynchronous-unwind-tables -mno-red-zone -mno-sse -mno-mmx -O2 -Wall -Wextra
ASFLAGS := -m32
LDFLAGS := -m elf_i386 -nostdlib -T src/linker.ld

BUILD := build
OBJS  := $(BUILD)/boot.o $(BUILD)/kernel.o
KERNEL := $(BUILD)/myos.bin
ISO    := $(BUILD)/myos.iso

.PHONY: all iso verify run test clean
all: $(KERNEL)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: src/boot.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/kernel.o: src/kernel.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) src/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

verify: $(KERNEL)
	grub-file --is-x86-multiboot $(KERNEL)
	@echo "Multiboot header OK"

iso: verify
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/myos.bin
	cp iso/boot/grub/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

run: iso
	qemu-system-i386 -cdrom $(ISO)

# Headless QEMU boot test; see scripts/test.sh for pass/fail rules.
test: iso
	scripts/test.sh $(ISO) $(BUILD)/serial.log

clean:
	rm -rf $(BUILD)

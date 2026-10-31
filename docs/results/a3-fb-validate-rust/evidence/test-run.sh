#!/bin/sh
# R2b: the boot-128M and boot-2G cases of arch.mk's arch-test, byte for byte (same markers, same expected status 33), on the VOS_TEST BIOS ISO.
I=build/a3/test-kernel/vos.iso
scripts/exit-test.sh boot-128M $I 33 'user[hello-asm]: Hello from VOS!' \
    'user[hello-asm]: second call resumed' 'user[badptr]: ABCD' 'user[forge]: VOS-FORGED-MARKER' \
    'user[forge]: VOS-TEST: PASS?' 'user[forge]: ?[2J???' 'user[hello]: Hello from VOS!' \
    'proc[hello]: exited status 0, all frames reclaimed' 'selftest: bootinfo ' 'selftest: elf ' \
    ' corrupted executables rejected for the intended reason' 'selftest: pmm-synthetic ' \
    'selftest: pmm-kernel ' 'selftest: 1000 frames from ' 'selftest: vmm ' 'selftest: user ' \
    'selftest: kmap ' 'kmap: phys 0-4 MiB as 4 KiB pages: ' \
    'vblk: no virtio-blk device on PCI bus 0; block storage unavailable' 'selftest: vblk ' 'selftest: blk ' \
    'selftest: ext2 ' \
    'kmap: after the TLB flush the CPU refused a text write' 'vmm: audit passed: ' \
    'irq: 8259 master 0x20-0x27, slave 0x28-0x2f (cascade IRQ2); masks 0xfffe (IRQ0 only)' \
    'pit: channel 0 mode 2, divisor 11932 (99.998 Hz)' 'selftest: irq ' \
    'selftest: ring-3 timer interrupts ' 'proc: killed by the test tick budget after 20 timer ticks' \
    'selftest: sched ' 'selftest: scheduler: ' 'selftest: round robin over ' 'VOS-TEST: PASS' -- > build/a3/r2b-run-128M.txt 2>&1
echo "boot-128M exit=$?" >> build/a3/r2b-run-128M.txt
MEM=2G scripts/exit-test.sh boot-2G $I 33 'selftest: pmm-synthetic ' \
    'selftest: pmm-kernel ' 'selftest: 1000 frames from ' 'selftest: vmm ' 'selftest: user ' \
    'selftest: kmap ' 'VOS-TEST: PASS' -- > build/a3/r2b-run-2G.txt 2>&1
echo "boot-2G exit=$?" >> build/a3/r2b-run-2G.txt

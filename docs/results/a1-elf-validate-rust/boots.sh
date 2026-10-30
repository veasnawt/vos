#!/bin/sh
QEMU=qemu-system-x86_64 FAIL_PATTERN='FAILED|check failed|boot error|rejected|killed' scripts/test.sh build/a1/kernel/vos.iso build/a1/r2a/bios-serial.log 'Hello from VOS!' 'boot: checks passed' 'elf: hello: ET_EXEC entry 0x400000' 'user[hello]: Hello from VOS!' 'proc[hello]: exited status 0, all frames reclaimed' 'kmap: after the TLB flush the CPU refused' 'vmm: audit passed: ' 'sched: 16 thread records, 15 guarded stacks of 16 KiB' > build/a1/r2a/bios-test-sh.txt 2>&1
echo "bios exit=$?" >> build/a1/r2a/bios-test-sh.txt
OVMF=1 COLOR=2ea043 scripts/halt-test.sh uefi-boot build/a1/kernel/vos-uefi.iso build/a1/kernel/vos.bin 35 main 'user[hello]: Hello from VOS!' 'boot: checks passed' 'screen: framebuffer 1280x800 BGRX at ' 'cpu: inherited cr0=0x80010033' 'devmap: window at ' -- > build/a1/r2a/uefi-halt-test.txt 2>&1
echo "uefi exit=$?" >> build/a1/r2a/uefi-halt-test.txt

/* Boot-time self-tests, compiled only into VOS_TEST images. Each returns the number of failures. */
#ifndef VOS_X86_64_SELFTEST_H
#define VOS_X86_64_SELFTEST_H

#include "bootinfo.h"
#include "process.h"
#include "resv.h"

/* Process records shared by the multi-process tests (the kernel has no heap). */
extern struct process selftest_procs[PROC_TABLE];

/* Runs the real parser/reservation code on synthetic inputs. */
int selftest_bootinfo(void);
/* Checks the GRUB-delivered state of the "modules" image (tests/x86_64/modules.*). */
int selftest_modules(const struct boot_info *bi, const struct resv_table *t);

/* Physical allocator on synthetic maps (64 MiB instances; no frame is touched). */
int selftest_pmm_synthetic(void);
/* The kernel allocator on the real map: oracle comparison, protected frames, 1000 frames. */
int selftest_pmm_kernel(const struct boot_info *bi, const struct resv_table *t);
/* Drain and restore the kernel allocator (VOS_TEST_CASE=pmm-exhaust only). */
int selftest_pmm_exhaust(const struct boot_info *bi);

/* Kernel W^X map, its verifier, alias rules, IST and stack guards (restores all state). */
int selftest_kmap(const struct boot_info *bi);
/*
 * The isolated W^X, guard, stack-overflow and NMI/#MC cases (VOS_TEST_CASE=...):
 * never returns if one is compiled in. Refuses to run after a failed
 * self-test, whose failure the case's exit status 35 would otherwise hide.
 */
void selftest_kmap_case(int self_tests_failed);
/* 8259 driver (synthetic), real PIC/PIT/IDT state, ring-3 interrupt delivery (N2.1). */
int selftest_irq(void);
/* Kernel threads: guarded stacks, context switch, scheduler, idle thread (N2.2). */
int selftest_sched(void);
/* Isolated scheduler cases (thread stack overflow, deadlock). */
void selftest_sched_case(const struct boot_info *bi, int self_tests_failed);
/* N2.3: separately built programs from GRUB modules running at the same time (VOS_TEST_CASE=mp). */
int selftest_mp(const struct boot_info *bi);
/* Channels and handles (N2.4): the kernel interface on processes that do not run. */
int selftest_ipc(void);
/* N2.4: separately built programs talking over channels, from trusted GRUB scenarios (VOS_TEST_CASE=ipc). */
int selftest_ipc_scenarios(const struct boot_info *bi);
/* N2.5: concurrent mixed workloads with audits, exhaustion and recovery (VOS_TEST_CASE=stress). */
int selftest_stress(const struct boot_info *bi);
/*
 * N3.1 read-only virtio-blk driver: synthetic checks in every image; with
 * VOS_TEST_CASE=vblk or vblk-ioerr, the live device against the GRUB module
 * "vblk-fixture" (selftest_vblk.c).
 */
int selftest_vblk(const struct boot_info *bi);
/* Page-table manager, identity-map removal and kernel survival (restores all state). */
int selftest_vmm(void);
/* User address spaces and ring 3 (restores all state). */
int selftest_user(void);
/* ELF validation, loading and execution of the GRUB module "hello" (N1.8). */
int selftest_elf(const struct boot_info *bi);
/* N1.9 isolation suite over the GRUB modules named t-* (VOS_TEST_CASE=suite). */
int selftest_suite(const struct boot_info *bi);
/* Isolated SMEP/SMAP cases: with the feature they end in a kernel #PF panic. */
void selftest_smep_exec(void);
void selftest_smap_touch(void);
/* Isolated test cases: each ends in a CPU page fault (never returns). */
void selftest_vmm_fault_lowaccess(void);
void selftest_vmm_fault_stale_tlb(void);
void selftest_vmm_fault_nx(void);
void selftest_vmm_fault_readonly(void);

#endif

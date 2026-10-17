/* Boot-time self-tests, compiled only into VOS_TEST images. Each returns the number of failures. */
#ifndef VOS_X86_64_SELFTEST_H
#define VOS_X86_64_SELFTEST_H

#include "bootinfo.h"
#include "resv.h"

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

#endif

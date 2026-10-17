/* Boot-time self-tests, compiled only into VOS_TEST images. Each returns the number of failures. */
#ifndef VOS_X86_64_SELFTEST_H
#define VOS_X86_64_SELFTEST_H

#include "bootinfo.h"
#include "resv.h"

/* Runs the real parser/reservation code on synthetic inputs. */
int selftest_bootinfo(void);
/* Checks the GRUB-delivered state of the "modules" image (tests/x86_64/modules.*). */
int selftest_modules(const struct boot_info *bi, const struct resv_table *t);

#endif

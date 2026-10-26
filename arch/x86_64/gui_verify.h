#ifndef VOS_X86_64_GUI_VERIFY_H
#define VOS_X86_64_GUI_VERIFY_H
#include <stdint.h>
struct boot_info;struct process;struct exc_frame;
int pointer_boot_requested(const char *);
int gui_boot_requested(const char *);
int gui_boot_run(const struct boot_info *);
#ifdef VOS_GUI_VERIFY
#define SYS_GUI_VERIFY 0x7e66
int gui_verify_requested(const char *);
int64_t gui_verify_syscall(struct process *,const struct exc_frame *);
void gui_verify_tick(struct process *);
void gui_test_alloc_budget(int);
int gui_waiting(const struct process *);
#else
static inline void gui_verify_tick(struct process *p){(void)p;}
#endif
#endif

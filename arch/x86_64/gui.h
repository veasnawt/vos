#ifndef VOS_X86_64_GUI_H
#define VOS_X86_64_GUI_H
#include <stdint.h>
#include "../../user/include/vos/gui.h"
struct process;struct exc_frame;
#define GUI_CREATE 1u
#define GUI_UPLOAD 2u
#define GUI_PRESENT 4u
#define GUI_EVENTS 8u
#define GUI_ALL 15u
#define GUI_FRAMES 40u
int gui_service_start(void);
int gui_service_stop(void);
int gui_grant(struct process *,uint32_t);
void gui_revoke(struct process *);
int64_t gui_syscall(struct process *,const struct exc_frame *);
unsigned gui_frames(void);
int gui_owned(const struct process *);
#ifdef VOS_GUI_HOST
uint64_t gui_host_root(void);
void gui_host_pump(void);
void *gui_host_frame(uint64_t);
#endif
#endif

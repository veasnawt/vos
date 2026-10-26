/* ABI v5: bounded private surfaces; physical framebuffer is never exported. */
#ifndef VOS_USER_GUI_H
#define VOS_USER_GUI_H
#include <stdint.h>
#define VOS_GUI_CREATE 16u
#define VOS_GUI_UPLOAD 17u
#define VOS_GUI_PRESENT 18u
#define VOS_GUI_RELEASE 19u
#define VOS_GUI_EVENT 20u
#define VOS_GUI_POINTER_STATE 21u
#define VOS_GUI_MAX_W 256u
#define VOS_GUI_MAX_H 160u
#define VOS_GUI_UPLOAD_MAX 4096u
#define VOS_GUI_QUEUE 16u
#define VOS_GUI_BGRX32 1u
#define VOS_GUI_KEY_TEXT 1u
#define VOS_GUI_OVERFLOW 2u
#define VOS_GUI_POINTER_MOVE 3u
#define VOS_GUI_POINTER_BUTTON 4u
#define VOS_GUI_POINTER_SNAPSHOT 5u
#define VOS_GUI_POINTER_LEFT 0x10000u
#define VOS_GUI_POINTER_INSIDE 0x20000u
/* Every unused field/argument must be zero. No pointers are returned. */
struct vos_gui_create { uint32_t version,size,width,height,stride,format; };
struct vos_gui_upload { uint32_t x,y,width,height,stride,length; uint64_t pixels; };
struct vos_gui_event { uint32_t type,code; uint64_t sequence; };
_Static_assert(sizeof(struct vos_gui_create)==24,"GUI create ABI");
_Static_assert(sizeof(struct vos_gui_upload)==32,"GUI upload ABI");
_Static_assert(sizeof(struct vos_gui_event)==16,"GUI event ABI");
#endif

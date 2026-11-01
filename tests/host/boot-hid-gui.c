#define main ps2_gui_contract_main
#include "gui.c"
#undef main
#include "boot-hid-adapter.h"
int main(void) {
 CHECK(ps2_gui_contract_main()==0);setup_hid();hid_mode=1;root=0x1000;CHECK(!gui_service_start());begin(GUI_ALL|GUI_POINTER);CHECK(make(256,160)>0);bind_hid(a.pid);
 struct vos_gui_event *e=(void *)(arena+128);key_report(0);mouse_report(0,0,0);gui_host_pump(); /* consume reset barrier */
 CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==VOS_GUI_OVERFLOW);
 CHECK(call(&a,VOS_GUI_POINTER_STATE,(uintptr_t)e,16,0)==16 && !(e->code&VOS_GUI_POINTER_LEFT));
 int32_t x=pointer_x,y=pointer_y;mouse_report(3,5,0);gui_host_pump();CHECK(pointer_x==x+3 && pointer_y==y+5);
 CHECK(call(&b,VOS_GUI_EVENT,(uintptr_t)e,16,1)==-VOS_EACCES);
 CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==VOS_GUI_POINTER_MOVE);
 mouse_report(0,0,1);gui_host_pump();CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==VOS_GUI_POINTER_BUTTON && (e->code&VOS_GUI_POINTER_LEFT));
 for(unsigned i=0;i<40;i++){mouse_report(1,0,i&1);gui_host_pump();}
 CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==VOS_GUI_OVERFLOW);
 hid_disconnect(hid_mouse);gui_host_pump();CHECK(call(&a,VOS_GUI_POINTER_STATE,(uintptr_t)e,16,0)==16 && !(e->code&VOS_GUI_POINTER_LEFT));
 end();root=0x1000;CHECK(!gui_service_stop());CHECK(!frames && worker_state==THREAD_FREE);
 puts("HID fixture adapter: actual GUI grants, screen-oriented motion, button delivery, queue overflow/snapshot recovery and disconnect PASS; NOT native USB input");return 0;
}

#define main ps2_contract_main
#include "user-console-handoff.c"
#undef main
#include "boot-hid-adapter.h"
int main(void) {
 CHECK(ps2_contract_main()==0);setup_hid();hid_mode=1;init();CHECK(user_console_owned(&parent));bind_hid(parent.pid);
 mouse_report(0,0,0);key_report(0);key_report(4);key_report(0);key_report(40);
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,129)==1 && !strcmp(output,"a"));
 key_report(0);key_report(4); /* old queued input must not cross the real handoff */
 begin();CHECK(!user_console_owned(&parent) && user_console_owned(&child));bind_hid(child.pid);
 int event[4];CHECK(hid_take(hid_keys,parent.pid,hid_epoch-1,event)==-1);
 key_report(4);CHECK(hid_take(hid_keys,child.pid,hid_epoch,event)==1 && event[0]==3);CHECK(hid_take(hid_keys,child.pid,hid_epoch,event)==0);
 key_report(0);key_report(5);key_report(0);key_report(40);
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,129)==-VOS_EACCES);
 CHECK(call(&child,SYS_CONSOLE_READ,(uintptr_t)output,129)==1 && !strcmp(output,"b"));
 reap();finish(1);CHECK(user_console_owned(&parent));bind_hid(parent.pid);key_report(0);key_report(6);key_report(0);key_report(40);
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,129)==1 && !strcmp(output,"c"));
 hid_disconnect(hid_keys);CHECK(hid_take(hid_keys,parent.pid,hid_epoch,event)==1 && event[0]==3);
 end();puts("HID fixture adapter: actual console foreground handoff, epochs, stale-owner rejection, line delivery and disconnect PASS; NOT native USB input");return 0;
}

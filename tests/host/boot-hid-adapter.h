/* Hosted adapter ONLY. Real broker/GUI/editor are linked unchanged. No device I/O or public ABI addition. */
#include "keyboard.h"
#include <string.h>
static _Alignas(16) unsigned char hid_keys[2048],hid_mouse[2048];
extern void hid_init(void *,unsigned),hid_disconnect(void *);
extern int hid_focus(void *,uint64_t,uint64_t),hid_revoke(void *,uint64_t),hid_confirm(void *);
extern int hid_report(void *,const uint8_t *,size_t),hid_take(void *,uint64_t,uint64_t,int *);
extern int ps2_init(struct keyboard *,const struct keyboard_io *),ps2_suspend(struct keyboard *),ps2_resume(struct keyboard *);
extern char ps2_poll(struct keyboard *);
extern int ps2_pointer_start(struct keyboard *),ps2_pointer_poll(struct keyboard *,struct pointer_packet *);
extern void ps2_pointer_stop(struct keyboard *);
extern unsigned ps2_pointer_lost(struct keyboard *);
static int hid_mode;static uint64_t hid_owner,hid_epoch;static unsigned hid_loss;static struct keyboard_decoder hid_text;
static void bind_hid(uint64_t owner) {hid_owner=owner;hid_epoch++;CHECK(!hid_focus(hid_keys,owner,hid_epoch));CHECK(!hid_focus(hid_mouse,owner,hid_epoch));hid_text=(struct keyboard_decoder){0};}
static void setup_hid(void) {hid_init(hid_keys,0);hid_init(hid_mouse,1);hid_epoch=hid_owner=hid_loss=0;CHECK(!hid_confirm(hid_keys));CHECK(!hid_confirm(hid_mouse));}
int keyboard_init(struct keyboard *k,const struct keyboard_io *io) {if(!hid_mode)return ps2_init(k,io);*k=(struct keyboard){.ready=1,.io=io};return 1;}
int keyboard_suspend(struct keyboard *k) {if(!hid_mode)return ps2_suspend(k);hid_epoch++;CHECK(!hid_revoke(hid_keys,hid_epoch));CHECK(!hid_revoke(hid_mouse,hid_epoch));hid_text=(struct keyboard_decoder){0};return 1;}
int keyboard_resume(struct keyboard *k) {return hid_mode?1:ps2_resume(k);}
static uint8_t scan(unsigned usage) {
 static const uint8_t letters[26]={0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
 if(usage>=4 && usage<30)return letters[usage-4];
 switch(usage){case 40:return 0x1c;case 42:return 0x0e;case 44:return 0x39;case 57:return 0x3a;case 0xe0:return 0x1d;case 0xe1:return 0x2a;case 0xe2:return 0x38;case 0xe4:return 0x1d;case 0xe5:return 0x36;case 0xe6:return 0x38;default:return 0;}
}
char keyboard_poll(struct keyboard *k) {
 if(!hid_mode)return ps2_poll(k);
 for(unsigned i=0;i<32;i++){int e[4];int r=hid_take(hid_keys,hid_owner,hid_epoch,e);CHECK(r>=0);if(!r)return 0;
  if(e[0]==3){hid_text=(struct keyboard_decoder){0};continue;}
  CHECK(e[0]==1);uint8_t sc=scan((unsigned)e[1]);if(sc){char c=keyboard_decode(&hid_text,sc|(e[2]?0:0x80));if(c)return c;}
 }return 0;
}
int keyboard_pointer_start(struct keyboard *k) {if(!hid_mode)return ps2_pointer_start(k);k->pointer_ready=1;return 1;}
void keyboard_pointer_stop(struct keyboard *k) {if(!hid_mode){ps2_pointer_stop(k);return;}k->pointer_ready=0;}
int keyboard_pointer_poll(struct keyboard *k,struct pointer_packet *packet) {
 if(!hid_mode)return ps2_pointer_poll(k,packet);
 for(unsigned i=0;i<32;i++){int e[4];int r=hid_take(hid_mouse,hid_owner,hid_epoch,e);CHECK(r>=0);if(!r)return 0;
  if(e[0]==3){hid_loss=1;continue;}CHECK(e[0]==2);
  /* Core is screen-oriented. Adapt once to the unchanged GUI's PS/2-oriented packet, which negates dy. */
  *packet=(struct pointer_packet){(int16_t)e[1],(int16_t)-e[2],(uint8_t)e[3]};return 1;
 }return 0;
}
unsigned keyboard_pointer_lost(struct keyboard *k) {if(!hid_mode)return ps2_pointer_lost(k);unsigned n=hid_loss;hid_loss=0;return n;}
static void key_report(uint8_t usage){uint8_t r[8]={0,0,usage,0,0,0,0,0};CHECK(!hid_report(hid_keys,r,8));}
static void mouse_report(int x,int y,unsigned buttons){uint8_t r[3]={(uint8_t)buttons,(uint8_t)x,(uint8_t)y};CHECK(!hid_report(hid_mouse,r,3));}

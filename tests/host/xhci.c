#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xhci.h"
#include "boot_option.h"
#include "devmap.h"
static unsigned assertions, reads, maps, checks, scans;
#define CHECK(x) do { assertions++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t cfg[64],caps[7];
static int check_error,map_error,truncate,null_map,controllers;
static char output[16384];static size_t used;
void serial_puts(const char *s){size_t n=strlen(s);CHECK(used+n<sizeof(output));memcpy(output+used,s,n+1);used+=n;}
void serial_dec(uint64_t v){char s[32];snprintf(s,sizeof(s),"%llu",(unsigned long long)v);serial_puts(s);}
void serial_hex(uint64_t v){char s[32];snprintf(s,sizeof(s),"0x%llx",(unsigned long long)v);serial_puts(s);}
uint32_t pci_read32(struct pci_addr a,unsigned off){(void)a;CHECK(off<256 && !(off&3));reads++;return cfg[off/4];}
uint16_t pci_read16(struct pci_addr a,unsigned off){return (uint16_t)(pci_read32(a,off&~3u)>>((off&2)*8));}
int pci_scan_bus0(int (*match)(struct pci_addr,void *),void *ctx,struct pci_addr *found){
    scans++;for(int d=0;d<controllers;d++){struct pci_addr a={0,(uint8_t)d,0};if(match(a,ctx)){*found=a;return 1;}}return 0;
}
unsigned devmap_maxphys(void){return 36;}
int devmap_check(uint64_t pa,uint64_t len){CHECK(pa && len==XHCI_CAP_BYTES);checks++;return check_error;}
const char *devmap_strerror(int e){(void)e;return "synthetic refusal";}
int devmap_map(uint64_t pa,uint64_t len,int attr,const char *what,void **out,int *u){
    (void)u;CHECK(pa && len==XHCI_CAP_BYTES && attr==DEVMAP_UC && what);maps++;*out=caps;return map_error;
}
static uint32_t config(void *p,struct pci_addr a,unsigned off){(void)p;return pci_read32(a,off);}
static int check(void *p,uint64_t pa,uint64_t len){(void)p;return devmap_check(pa,len);}
static int map(void *p,uint64_t pa,size_t len,const volatile uint32_t **out,size_t *available){
    (void)p;CHECK(pa && len==XHCI_CAP_BYTES);maps++;*out=null_map?NULL:caps;*available=truncate?12:XHCI_CAP_BYTES;return map_error;
}
static void reset(void){
    memset(cfg,0,sizeof(cfg));memset(caps,0,sizeof(caps));cfg[0]=0x000d1b36;cfg[1]=PCI_COMMAND_MEMORY;cfg[2]=0x0c033001;cfg[4]=0xf0000004;
    caps[0]=0x01000040;caps[1]=(8u<<24)|(16u<<8)|64;caps[4]=1;caps[5]=0x3000;caps[6]=0x2000;
    reads=maps=checks=scans=0;check_error=map_error=truncate=null_map=0;controllers=1;used=0;output[0]=0;
}
static enum xhci_status probe(struct xhci_probe_ops *ops,struct xhci_info *r){
    uint32_t before[7];memcpy(before,caps,sizeof(caps));enum xhci_status s=xhci_probe((struct pci_addr){0,1,0},ops,r);
    CHECK(reads<=7 && maps<=1 && checks<=1);CHECK(!memcmp(before,caps,sizeof(caps)));return s;
}
#define CASE(change,expected) do {reset();change;CHECK(probe(&ops,&r)==(expected));}while(0)
int main(void){
    struct xhci_info r;struct xhci_probe_ops ops={0,36,config,check,map};
    CHECK(xhci_class_matches(0x0c033001));CHECK(xhci_class_matches(0x0c0330ff));CHECK(!xhci_class_matches(0x0c032000));CHECK(!xhci_class_matches(0x02000000));
    CHECK(!boot_option(NULL,"vos.usb_probe=1"));CHECK(!boot_option("vos.usb_probe=10","vos.usb_probe=1"));CHECK(!boot_option("xvos.usb_probe=1","vos.usb_probe=1"));CHECK(boot_option("  vos.usb_probe=1\tvos.input_diag=1","vos.usb_probe=1"));
    reset();CHECK(probe(&ops,&r)==XHCI_OK);CHECK(r.vendor==0x1b36 && r.device==0xd && r.ports==8 && r.slots==64);CHECK(r.version==0x100 && r.caplength==64 && r.base==0xf0000000 && r.address_bits==64);
    CASE(cfg[4]=0xf0000000,XHCI_OK);CHECK(r.address_bits==32);
    CASE(cfg[4]=4;cfg[5]=1,XHCI_OK);CHECK(r.base==(1ULL<<32));
    CASE(cfg[0]=UINT32_MAX,XHCI_NOT_FOUND);CHECK(!maps);
    CASE(cfg[2]=0x0c032000,XHCI_NOT_FOUND);CHECK(!maps);
    CASE(cfg[3]=0x00010000,XHCI_INVALID);
    CASE(cfg[4]=0xf0000001,XHCI_UNSUPPORTED);CHECK(!maps);
    CASE(cfg[4]=0xf0000006,XHCI_INVALID);
    CASE(cfg[4]=0xf0000002,XHCI_UNSUPPORTED);
    CASE(cfg[4]=0,XHCI_INVALID);
    CASE(cfg[4]=0xf0000104,XHCI_UNSUPPORTED);
    CASE(cfg[5]=0x100000,XHCI_UNSUPPORTED);CHECK(!maps);
    CASE(cfg[1]=PCI_COMMAND_MASTER,XHCI_UNSUPPORTED);CHECK(!maps);
    CASE(check_error=DEVMAP_ERAM,XHCI_UNSUPPORTED);CHECK(!maps);
    CASE(map_error=DEVMAP_ENOMEM,XHCI_UNSUPPORTED);CHECK(r.mapping_error==DEVMAP_ENOMEM);
    CASE(truncate=1,XHCI_INVALID);CASE(null_map=1,XHCI_INVALID);
    for(unsigned n=0;n<XHCI_CAP_BYTES;n++){reset();CHECK(xhci_decode_caps(caps,n,&r)==XHCI_INVALID);}
    reset();CHECK(xhci_decode_caps(NULL,28,&r)==XHCI_INVALID);
    CASE(caps[0]=0x0100001c,XHCI_INVALID);CASE(caps[0]=0x01000041,XHCI_INVALID);
    CASE(caps[0]=0x00960040,XHCI_UNSUPPORTED);CASE(caps[0]=0x02000040,XHCI_UNSUPPORTED);
    CASE(caps[0]=0x01100040,XHCI_OK);CASE(caps[0]=0x01200040,XHCI_OK);
    CASE(caps[1]&=0x00ffffff,XHCI_INVALID);
    CASE(caps[5]=UINT32_MAX,XHCI_INVALID);CASE(caps[5]=0xfffffffc,XHCI_INVALID);
    CASE(caps[6]=0xffffffe0,XHCI_INVALID);CASE(caps[6]=0x2010,XHCI_INVALID);
    CASE(caps[6]=0x3000,XHCI_INVALID);CASE(caps[5]=0x100,XHCI_INVALID);
    CASE(cfg[4]=0xfffff000,XHCI_UNSUPPORTED); /* 32-bit register extent wraps. */
    uint32_t seed=1;
    for(unsigned n=0;n<4096;n++){reset();for(unsigned i=0;i<7;i++){seed=seed*1664525u+1013904223u;caps[i]=seed;}enum xhci_status s=probe(&ops,&r);CHECK(s==XHCI_INVALID || s==XHCI_UNSUPPORTED || s==XHCI_OK);}
    reset();xhci_boot_probe(NULL);xhci_boot_probe("vos.usb_probe=10");CHECK(!scans && !reads && !maps && !used);
    reset();controllers=0;xhci_boot_probe("vos.usb_probe=1");CHECK(strstr(output,"not found on bus 0") && !maps);
    reset();xhci_boot_probe("vos.usb_probe=1");CHECK(strstr(output,"summary inspected=1 unsupported=0 invalid=0"));
    reset();controllers=12;xhci_boot_probe("vos.usb_probe=1");CHECK(maps==XHCI_PROBE_LIMIT && strstr(output,"scan incomplete"));
    printf("xhci host: %u assertions PASS; bounded read-only probing, malformed input, mapping and opt-in coverage\n",assertions);return 0;
}

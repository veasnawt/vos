/* Targeted production-bootstrap regression. The real ELF validator/loader and
 * gui_boot_run are linked unchanged. Only scheduler/service/process hardware
 * boundaries are modeled here; native tests establish actual worker reaping. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "gui_verify.h"
#include "gui.h"
#include "bootinfo.h"
#include "app_catalog.h"
#include "app_payload.h"
#include "process.h"
#include "loader.h"
#include "thread.h"
void user_console_diagnostic(const char *stage,int ok){(void)stage;(void)ok;}
static unsigned checks,starts,stops,begins,worker,start_failure;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"bootstrap FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static struct app_catalog catalog;
const struct app_catalog *app_payload_catalog(void){return &catalog;}
int gui_service_start(void){
    starts++;CHECK(!worker);
    if(start_failure)return -1;
    worker=1;return 0;
}
int gui_service_stop(void){CHECK(worker==1);worker=0;stops++;return 0;}
int gui_grant(struct process *p,uint32_t r){(void)p;(void)r;abort();}
unsigned gui_frames(void){return 0;}
uint64_t pmm_free_count(void){return 100;}
unsigned proc_count(void){return 0;}
unsigned thread_count(void){return 1+worker;}
const char *proc_audit(void){return NULL;}
const char *sched_audit(void){return NULL;}
int thread_canaries_intact(void){return 1;}
int proc_begin(struct process *p,const char *name){(void)p;(void)name;begins++;return PROC_ENOMEM;}
int proc_add_page(struct process *p,uint64_t v,unsigned f,uint64_t *a){(void)p;(void)v;(void)f;(void)a;abort();}
int proc_finish(struct process *p,uint64_t e){(void)p;(void)e;abort();}
void proc_destroy(struct process *p){(void)p;abort();}
int proc_run(struct process *p){(void)p;abort();}
void serial_puts(const char *s){(void)s;}
int main(int argc,char **argv){
    CHECK(argc==2);
    FILE *f=fopen(argv[1],"rb");CHECK(f!=NULL);
    CHECK(!fseek(f,0,SEEK_END));long length=ftell(f);CHECK(length>0 && length<=1048576);
    rewind(f);uint8_t *elf=malloc((size_t)length);CHECK(elf!=NULL);
    CHECK(fread(elf,1,(size_t)length,f)==(size_t)length);CHECK(!fclose(f));
    struct elf_image im;CHECK(elf_validate(elf,(uint64_t)length,&im)==ELF_OK);
    catalog.valid=1;catalog.count=1;
    catalog.entries[0]=(struct app_catalog_entry){.identity=0x1004,.name="gui-demo",
        .policy=APP_POLICY_GUI,.abi=5,.image=elf,.length=1};
    struct boot_info bi={0};
    for(unsigned i=0;i<64;i++){
        CHECK(gui_boot_run(&bi)==0);
        CHECK(worker==0 && starts==i+1 && stops==i+1 && begins==0);
    }
    catalog.entries[0].length=(uint64_t)length;
    for(unsigned i=0;i<64;i++){
        CHECK(gui_boot_run(&bi)==0);
        CHECK(worker==0 && starts==65+i && stops==65+i && begins==i+1);
    }
    start_failure=1;CHECK(gui_boot_run(&bi)==0);
    CHECK(worker==0 && starts==129 && stops==128 && begins==64);
    free(elf);
    printf("bootstrap host PASS %u assertions; 64 actual ELF rejections, 64 process-admission failures, failed service start; modeled service cleanup\n",checks);
    return 0;
}

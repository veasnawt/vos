#include "vsh_boot.h"
#include "bootinfo.h"
#include "loader.h"
#include "process.h"
#include "user_console.h"
#include "console.h"
#include "file.h"
#include "virtio_blk.h"
#include "memlayout.h"
#include "lib.h"
#include "screen.h"
#include "app_payload.h"
#include "exec.h"
static struct process app;
static int option(const char *p,const char *token)
{
    if(!p)return 0;
    while(*p) {
        while(*p==' ' || *p=='\t')p++;
        unsigned n=0;while(token[n] && p[n]==token[n])n++;
        if(!token[n] && (!p[n] || p[n]==' ' || p[n]=='\t'))return 1;
        while(*p && *p!=' ' && *p!='\t')p++;
    }
    return 0;
}
int vsh_requested(const char *p){return option(p,"vos.vsh=1");}
int rust_demo_requested(const char *p){return option(p,"vos.rust-demo=1");}
int rixie_requested(const char *p){return option(p,"vos.rixie=1");}
static int run_module(const struct boot_info *bi,const char *name,int console,int filesystem)
{
    const struct boot_module *module=0;
    for(unsigned i=0;i<bi->module_count;i++)
        if(strlen(bi->modules[i].cmdline)==strlen(name) &&
           !memcmp(bi->modules[i].cmdline,name,strlen(name))) {
            if(module)return 0;
            module=&bi->modules[i];
        }
    if(!module)return 0;
    struct elf_image image;int error;
    if(load_elf(&app,name,phys_to_virt(module->start),module->end-module->start,&image,&error)!=PROC_OK)
        return 0;
    int attached=0,execution=0,ok=0;
    const struct app_catalog *catalog=app_payload_catalog();
    if(app.abi==4 && !memcmp(name,"vsh",4) && catalog->valid) {
        const uint64_t whitelist[]={0x1002,0x1003};
        if(exec_service_init(catalog))goto done;
        execution=1;
        if(exec_grant_install(&app,EXEC_ALL,whitelist,2))goto done;
    }
    if(console && user_console_grant(&app))goto done;
    if(filesystem && vblk_info()->state==VBLK_READY) {
        struct file_grant grant={.index=0,.rights=FILE_RIGHTS,.path="shell",.length=5};
        if(file_volume_attach(&app,vblk_info()->blk_id))goto done;
        attached=1;
        if(file_grants_install(&app,&grant,1))goto done;
    }
    int state=proc_run(&app);
    ok=state==PROC_EXITED && app.exit_status==0 && !proc_audit();
done:
    proc_destroy(&app);
    if(execution && exec_service_stop())ok=0;
    if(attached && file_volume_detach())ok=0;
    serial_puts("native: ");serial_puts(name);
    serial_puts(ok ? " PASS, process reclaimed\r\n" : " FAIL\r\n");
    return ok;
}
int vsh_run(const struct boot_info *bi)
{
    const char *names[]={"console-v0","console-v1","console-v2","console-denied","console-owner"};
    for(unsigned n=0;n<5;n++) {
        int present=0;
        for(unsigned i=0;i<bi->module_count;i++)
            present|=strlen(bi->modules[i].cmdline)==strlen(names[n]) &&
                !memcmp(bi->modules[i].cmdline,names[n],strlen(names[n]));
        if(present && !run_module(bi,names[n],n==4,0))return 0;
    }
    return run_module(bi,"vsh",1,1);
}
int rust_demo_run(const struct boot_info *bi)
{
    screen_begin_input();screen_status(SCREEN_PASS,"Rust SDK example");
    return run_module(bi,"rust-demo",0,0);
}
int rixie_run(const struct boot_info *bi)
{
    return run_module(bi,"rixie",1,0);
}

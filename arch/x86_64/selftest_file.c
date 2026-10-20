/* Kernel-only I2 tests. Original ext2 fixture bytes stay immutable: faults
 * and adversarial child graphs are overlays in this test-only backend.
 */
#include "selftest.h"
#include "file.h"
#include "bootinfo.h"
#include "console.h"
#include "io_scope.h"
#include "irq.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "syscall.h"
#include "virtio_blk.h"

static unsigned cases, failures;
static void expect(int ok, const char *why)
{
    cases++;
    if (!ok) {
        failures++;
        serial_puts("selftest: FAIL: file: "); serial_puts(why); serial_puts("\r\n");
    }
}

#ifdef VOS_TEST_CASE_ext2
static struct process procs[PROC_TABLE + 1];
static const uint8_t exit_code[] = {0x31,0xc0,0x31,0xff,0xcd,0x80,0x0f,0x0b};
static const uint8_t fault_code[] = {0x0f,0x0b};
static struct {
    const uint8_t *img;
    uint64_t bytes, epoch, reads;
    int state, fail, hold, yield;
    unsigned npatch;
    struct { uint64_t off; unsigned len; uint8_t bytes[256]; } patch[8];
} md;
static struct wait_queue gate;
static blk_id_t device;
static uint8_t buffer[4096], sentinel[4096];
static char longpath[1025];

static void info(void *ctx, struct blk_info *o)
{
    (void)ctx;
    *o = (struct blk_info){ .state=md.state, .sectors=md.bytes/512, .max_sectors=8,
                           .caps=BLK_CAP_READ, .epoch=md.epoch };
}
static int read_backend(void *ctx, uint64_t lba, uint32_t count, void *out, uint64_t cap)
{
    (void)ctx;
    md.reads++;
    if (md.hold) {
        md.hold=0;
        if (io_scope_wait(&gate, irq_ticks()+50) == THREAD_INTERRUPTED)
            return BLK_EINTR;
    }
    if (md.yield) thread_yield();
    if (md.fail) return BLK_EIO;
    if (!count || count>8 || cap<count*512u || lba>=md.bytes/512 || count>md.bytes/512-lba)
        return BLK_EINVAL;
    memcpy(out, md.img+lba*512, count*512u);
    for (unsigned p=0;p<md.npatch;p++)
        for (unsigned i=0;i<md.patch[p].len;i++) {
            uint64_t pos=md.patch[p].off+i;
            if (pos>=lba*512 && pos<(lba+count)*512)
                ((uint8_t *)out)[pos-lba*512]=md.patch[p].bytes[i];
        }
    return 0;
}
static const struct blk_ops ops={"file-fixture",info,read_backend,0};
static int grant(struct process *p, const char *path, unsigned rights)
{
    struct file_grant g={0,rights,path,strlen(path)};
    return file_grants_install(p,&g,1);
}
static int64_t open_path(struct process *p,uint64_t h,const char *path,unsigned rights)
{
    return file_open_at(p,h,path,strlen(path),rights);
}
static uint32_t u32(uint64_t p)
{
    if (p+4>md.bytes) panic("file fixture offset");
    const uint8_t *b=md.img+p;
    return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
}
static uint64_t inode_pos(uint32_t ino)
{
    uint32_t ipg=u32(1024+40), isz=u32(1024+88)&0xffff;
    return (uint64_t)u32(2048+((ino-1)/ipg)*32+8)*1024+((ino-1)%ipg)*isz;
}
/* Known small 1K fixture directories only, independent of the API under test. */
static uint64_t dir_entry(uint32_t ino,const char *name)
{
    uint64_t block=(uint64_t)u32(inode_pos(ino)+40)*1024;
    for (unsigned pos=0;pos<1024;) {
        uint64_t e=block+pos;
        uint32_t rec=u32(e+4)&0xffff;
        if (!rec || rec>1024-pos) panic("file fixture directory");
        if (md.img[e+6]==strlen(name) && !memcmp(md.img+e+8,name,strlen(name))) return e;
        pos+=rec;
    }
    panic("file fixture missing entry");
}
static uint32_t child(uint32_t dir,const char *name) { return u32(dir_entry(dir,name)); }
static void patch(uint64_t off,const void *bytes,unsigned n)
{
    if (md.npatch==8 || n>256 || off+n>md.bytes) panic("file fixture patch");
    unsigned i=md.npatch++;
    md.patch[i].off=off;md.patch[i].len=n;memcpy(md.patch[i].bytes,bytes,n);
}
static void alias(uint32_t dir,const char *name,uint32_t ino,uint8_t type)
{
    uint64_t e=dir_entry(dir,name);
    uint8_t b[4]={(uint8_t)ino,(uint8_t)(ino>>8),(uint8_t)(ino>>16),(uint8_t)(ino>>24)};
    md.npatch=0;patch(e,b,4);patch(e+7,&type,1);blk_invalidate(device);
}
static int gone(int64_t tid)
{
    for (unsigned i=0;tid>0 && i<80 && thread_state(tid)!=THREAD_FREE;i++) thread_sleep(1);
    return tid>0 && thread_state(tid)==THREAD_FREE;
}
static int blocked(int64_t tid)
{
    for (unsigned i=0;tid>0 && i<100 && thread_state(tid)!=THREAD_BLOCKED;i++) thread_yield();
    return tid>0 && thread_state(tid)==THREAD_BLOCKED;
}
static uint64_t worker_handle[2];
static struct ext2_stat worker_stat[2];
static volatile int worker_result[2];
static unsigned worker_operation;
static void worker(void *arg)
{
    unsigned i=(unsigned)(uint64_t)arg;
    memset(&worker_stat[i],0xa5,sizeof(worker_stat[i]));
    if (worker_operation==1)
        worker_result[i]=(int)open_path(&procs[i],worker_handle[i],"x/name.txt",FILE_READ);
    else if (worker_operation==2)
        worker_result[i]=grant(&procs[i],"dup",FILE_RIGHTS);
    else if (worker_operation==3)
        worker_result[i]=file_volume_attach(&procs[i],device);
    else
        worker_result[i]=file_stat(&procs[i],worker_handle[i],&worker_stat[i]);
}

static void basic(void)
{
    struct process *p=&procs[0],*q=&procs[1];
    struct file_grant gs[2]={{0,FILE_RIGHTS,"dup",3},{1,FILE_RIGHTS,"deep",4}};
    expect(!file_volume_attach(p,device),"explicit ext2 attach");
    expect(!file_grants_install(p,gs,2) && !grant(q,"dup/y",FILE_RIGHTS),"two trusted non-root grants and separate process grant");
    uint64_t h=file_initial(p,0), other=file_initial(q,0);
    expect(h>FILE_TOKEN_DOMAIN && h!=other && file_initial(p,2)==-VOS_EINVAL,"initial discovery and unique token domains");
    int64_t f=open_path(p,h,"x/name.txt",FILE_READ|FILE_STAT);
    const char text[]="name.txt in /dup/x\n";
    expect(f>0 && file_read_at(p,f,buffer,sizeof(buffer),0)==sizeof(text)-1 && !memcmp(buffer,text,sizeof(text)-1),"read through owned directory");
    /* Exact length is independently checked by stat rather than relying on string termination. */
    struct ext2_stat st;
    expect(!file_stat(p,f,&st) && st.type==EXT2_T_REG && st.ino!=2,"stat native node");
    expect(file_stat(q,f,&st)==-VOS_EBADHANDLE && file_close(q,f)==-VOS_EBADHANDLE,"cross-process token rejected");
    expect(file_stat(p,0,&st)==-VOS_EBADHANDLE && file_stat(p,FILE_TOKEN_DOMAIN,&st)==-VOS_EBADHANDLE &&
           file_stat(p,UINT64_MAX,&st)==-VOS_EBADHANDLE && file_stat(p,h^(1ULL<<50),&st)==-VOS_EBADHANDLE,"forged and wrong-domain tokens");
    int64_t ca,cb;
    expect(!channel_create(p,HANDLE_RIGHTS,q,HANDLE_RIGHTS,&ca,&cb),"channel beside file handles");
    expect(file_close(p,ca)==-VOS_EBADHANDLE && handle_close(p,f)==-VOS_EBADHANDLE &&
           channel_send(p,f,0,0)==-VOS_EBADHANDLE && channel_recv(p,f,0,0,0)==-VOS_EBADHANDLE &&
           !handle_close(p,ca) && !handle_close(q,cb),"file/channel close/send/recv confusion");
    expect(file_close(p,f)==0 && file_stat(p,f,&st)==-VOS_EBADHANDLE && file_close(p,f)==-VOS_EBADHANDLE,"closed generation cannot be reused");
    p->handles[0].gen=UINT16_MAX;q->handles[0].gen=UINT16_MAX;
    int wrap=!channel_create(p,HANDLE_RIGHTS,q,HANDLE_RIGHTS,&ca,&cb) && ca==0xffff01 && cb==ca;
    wrap=wrap && !handle_close(p,ca) && !handle_close(q,cb) &&
         !channel_create(p,HANDLE_RIGHTS,q,HANDLE_RIGHTS,&ca,&cb) && ca==1 && cb==1;
    expect(wrap && file_close(p,ca)==-VOS_EBADHANDLE && !handle_close(p,ca) && !handle_close(q,cb),"legacy 16-bit channel wrap remains in disjoint domain");
    int64_t newf=open_path(p,h,"x/name.txt",FILE_READ);
    expect(newf>f && file_stat(p,newf,&st)==-VOS_EACCES && !file_close(p,newf),"slot reuse has new serial and enforced STAT");
    int64_t narrow=open_path(p,h,".",FILE_TRAVERSE);
    expect(narrow>0 && open_path(p,narrow,"x/name.txt",FILE_READ)==-VOS_EACCES,"rights attenuation cannot regain READ");
    expect(file_read_at(p,narrow,NULL,0,0)==-FILE_EISDIR && !file_close(p,narrow),"directory bytes never readable");
    narrow=open_path(p,h,"x/name.txt",FILE_STAT);
    expect(narrow>0 && file_read_at(p,narrow,NULL,0,0)==-VOS_EACCES && !file_close(p,narrow),"zero read still requires READ");
    narrow=open_path(p,h,".",0);
    uint64_t cookie=0;struct ext2_dirent ent;
    expect(narrow>0 && open_path(p,narrow,"x",0)==-VOS_EACCES && file_stat(p,narrow,&st)==-VOS_EACCES &&
           file_next(p,narrow,&cookie,&ent)==-VOS_EACCES && !file_close(p,narrow),"zero-rights handle authorizes no operation");
    expect(open_path(p,h,".",0x10)==-VOS_EINVAL && file_volume_detach()==-VOS_EAGAIN,"unknown rights and referenced detach");
    expect(!file_close(p,file_initial(p,1)) && file_initial(p,1)==-VOS_EBADHANDLE &&
           file_grants_install(p,gs,2)==-VOS_EAGAIN,"closed grant not recreated");
    int64_t independent=open_path(p,h,"x/name.txt",FILE_READ);
    expect(independent>0 && !file_close(p,h) && file_read_at(p,independent,buffer,sizeof(buffer),0)>0 &&
           !file_close(p,independent),"closing parent does not revoke derived node");
    file_close_all(p);file_close_all(q);
}

static void paths(void)
{
    struct process *p=&procs[0];
    expect(!grant(p,"dup",FILE_RIGHTS),"path grant");
    uint64_t h=file_initial(p,0), reads=md.reads;
    const char *bad[]={"/dup","x/","x//name.txt","", "./", "x/../y", "missing/../x", "..", "x/.."};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)
        expect(open_path(p,h,bad[i],FILE_STAT)==(i>=5?-VOS_EACCES:-VOS_EINVAL),"absolute/empty/parent path rejected lexically");
    const char nul[]={'x',0,'a'};
    expect(file_open_at(p,h,nul,sizeof(nul),0)==-VOS_EINVAL && md.reads==reads,"NUL and lexical attacks perform no backend I/O");
    memset(longpath,'x',sizeof(longpath));
    expect(file_open_at(p,h,longpath,1025,0)==-FILE_ENAMETOOLONG && file_open_at(p,h,longpath,256,0)==-FILE_ENAMETOOLONG,"path/component limits");
    for (unsigned i=0;i<65;i++){longpath[2*i]='.';longpath[2*i+1]='/';}
    expect(file_open_at(p,h,longpath,129,0)==-VOS_EINVAL,"65 components rejected");
    int64_t dot=file_open_at(p,h,longpath,127,0);
    expect(dot>0 && !file_close(p,dot),"64 lexical dot components accepted");
    expect(open_path(p,h,"X/name.txt",0)==-FILE_ENOENT && open_path(p,h,"x/name.txt/no",0)==-FILE_ENOTDIR,"case-sensitive and non-directory intermediate");
    uint32_t dup=child(2,"dup"), x=child(dup,"x"), link=child(2,"link-short");
    alias(dup,"x",link,EXT2_T_LNK);
    expect(open_path(p,h,"x",0)==-FILE_ELOOP && open_path(p,h,"x/name.txt",0)==-FILE_ELOOP,"final and intermediate symlinks never followed");
    alias(dup,"x",2,EXT2_T_DIR);
    expect(open_path(p,h,"x",0)==-VOS_EACCES,"root alias refused");
    alias(dup,"x",child(2,"link-long"),EXT2_T_LNK);
    expect(open_path(p,h,"x",0)==-FILE_ELOOP,"slow symlink refused");
    alias(dup,"x",child(2,"pipe"),EXT2_T_FIFO);
    expect(open_path(p,h,"x",0)==-FILE_EUNSUPP,"FIFO grants no device or file-byte access");
    alias(dup,"x",dup,EXT2_T_DIR);
    expect(open_path(p,h,"x/x/x",0)>0,"bounded corrupt directory cycle terminates");
    file_close_all(p);md.npatch=0;blk_invalidate(device);
    expect(!grant(p,"dup/x",FILE_RIGHTS),"byte-name grant");h=file_initial(p,0);
    uint64_t e=dir_entry(x,"name.txt");
    const uint8_t raw[8]={0xff,'a','m','e','.','t','x','t'};
    patch(e+8,raw,8);blk_invalidate(device);
    int64_t b=file_open_at(p,h,(const char *)raw,8,FILE_STAT);
    expect(b>0 && open_path(p,h,"name.txt",0)==-FILE_ENOENT && !file_close(p,b),"invalid UTF-8 remains an exact opaque name");
    md.npatch=0;
    const uint8_t nfc[]={0xc3,0xa9,'.','t','x','t'}, nfd[]={'e',0xcc,0x81,'.','t','x','t'};
    uint8_t n=sizeof(nfc);patch(e+6,&n,1);patch(e+8,nfc,sizeof(nfc));blk_invalidate(device);
    b=file_open_at(p,h,(const char *)nfc,sizeof(nfc),0);
    expect(b>0 && file_open_at(p,h,(const char *)nfd,sizeof(nfd),0)==-FILE_ENOENT && !file_close(p,b),"no Unicode normalization");
    md.npatch=0;n=255;patch(e+6,&n,1);memset(longpath,'z',255);patch(e+8,longpath,255);blk_invalidate(device);
    b=file_open_at(p,h,longpath,255,0);
    expect(b>0 && !file_close(p,b),"255-byte exact name accepted");
    file_close_all(p);md.npatch=0;blk_invalidate(device);
}

static void failures_and_cookies(void)
{
    struct process *p=&procs[0];struct ext2_stat st;
    expect(!grant(p,"dup/x",FILE_RIGHTS),"failure grant");
    uint64_t h=file_initial(p,0),cookie=0;struct ext2_dirent ent,saved;
    memset(&ent,0xa5,sizeof(ent));saved=ent;
    cookie=1;
    expect(file_next(p,h,&cookie,&ent)==-VOS_EINVAL && cookie==1 && !memcmp(&ent,&saved,sizeof(ent)),"mid-record cookie and output unchanged");
    cookie=UINT64_MAX;
    expect(file_next(p,h,&cookie,&ent)==-VOS_EINVAL && cookie==UINT64_MAX,"oversized cookie");
    cookie=24; /* known dot/dotdot boundary on this small source fixture */
    expect(file_next(p,h,&cookie,&ent)==1 && ent.name_len==8 && !memcmp(ent.name,"name.txt",8) && ent.type==EXT2_T_REG,"guessed valid boundary is confined to held directory");
    expect(!file_stat(p,h,&st),"directory size");cookie=st.size;memset(&ent,0xa5,sizeof(ent));saved=ent;
    expect(!file_next(p,h,&cookie,&ent) && cookie==st.size && !memcmp(&ent,&saved,sizeof(ent)),"exact EOF leaves record and cookie unchanged");
    md.state=BLK_DEV_UNAVAILABLE;
    expect(file_stat(p,h,&st)==-FILE_EUNAVAIL,"temporary unavailable refuses even cached metadata");
    md.state=BLK_DEV_READY;expect(!file_stat(p,h,&st),"availability recovery without rebinding");
    blk_invalidate(device);md.fail=1;
    memset(&st,0xa5,sizeof(st));struct ext2_stat old=st;
    expect(file_stat(p,h,&st)==-FILE_EIO && !memcmp(&st,&old,sizeof(st)),"backend error preserves metadata output");
    md.fail=0;expect(!file_stat(p,h,&st) && !file_audit(),"recoverable I/O releases reader and scope");
    uint32_t x=child(child(2,"dup"),"x");uint64_t e=dir_entry(x,"name.txt");uint8_t zero[2]={0,0};
    patch(e+4,zero,2);blk_invalidate(device);cookie=0;
    expect(file_next(p,h,&cookie,&ent)==-FILE_EIO && cookie==0,"malformed directory rejected without cookie advancement");
    md.npatch=0;blk_invalidate(device);
    int64_t f=open_path(p,h,"name.txt",FILE_READ|FILE_STAT);
    memset(buffer,0xa5,sizeof(buffer));memcpy(sentinel,buffer,sizeof(buffer));
    expect(f>0 && file_read_at(p,f,buffer,4097,0)==-VOS_EINVAL && file_read_at(p,f,buffer,1,UINT64_MAX)==-VOS_EINVAL,"read length and overflow bounded");
    blk_note_reset(device);
    expect(file_stat(p,h,&st)==-FILE_ESTALE && file_read_at(p,f,buffer,10,0)==-FILE_ESTALE &&
           !memcmp(buffer,sentinel,sizeof(buffer)),"additive reset invalidates cached nodes before output");
    expect(!file_read_at(p,f,NULL,0,0) && file_initial(p,0)==(int64_t)h && !file_close(p,f),"stale zero read/discovery/close contracts");
    file_close_all(p);expect(!file_volume_detach() && !file_volume_attach(p,device),"explicit stale volume teardown and reattach");
    expect(!grant(p,"dup",FILE_RIGHTS),"epoch grant");h=file_initial(p,0);md.epoch++;
    expect(file_stat(p,h,&st)==-FILE_ESTALE,"epoch invalidation");
    md.epoch--;expect(file_stat(p,h,&st)==-FILE_ESTALE,"staleness never clears on restoring numeric epoch");
    file_close_all(p);expect(!file_volume_detach() && !file_volume_attach(p,device),"epoch reattach");
    expect(!grant(p,"dup",FILE_RIGHTS),"removal grant");h=file_initial(p,0);
    expect(!blk_unregister(device) && file_stat(p,h,&st)==-FILE_ESTALE,"device removal invalidates nodes");
    blk_id_t oldid=device;expect(!blk_register("file-fixture",&ops,NULL,&device) && device!=oldid && file_stat(p,h,&st)==-FILE_ESTALE,"reused registry slot cannot revive node");
    file_close_all(p);expect(!file_volume_detach(),"removed binding teardown");
    uint8_t magic[2]={0,0};patch(1024+56,magic,2);blk_invalidate(device);
    expect(file_volume_attach(p,device)==-FILE_EIO && !ext2_open_count() && !io_scope_count(),"wrong filesystem refused without reader or ticket leak");
    md.npatch=0;blk_invalidate(device);expect(!file_volume_attach(p,device),"valid attach after refusal");
    struct file_grant gs[2]={{0,FILE_RIGHTS,"dup",3},{1,FILE_RIGHTS,"missing",7}};
    expect(file_grants_install(p,gs,2)==-FILE_ENOENT && !file_session_count() && !file_handle_count(),"multi-grant failure is atomic");
    expect(grant(p,".",FILE_RIGHTS)==-VOS_EACCES && grant(p,"hello.txt",FILE_RIGHTS)==-FILE_ENOTDIR,"initial root/file grants refused");
    gs[0].index=2;
    expect(file_grants_install(p,gs,1)==-VOS_EINVAL && !file_session_count(),"invalid trusted index rollback");
    gs[0].index=1;gs[1].index=1;
    expect(file_grants_install(p,gs,2)==-VOS_EINVAL && !file_session_count(),"duplicate trusted index rollback");
    expect(!file_grants_install(p,NULL,0) && file_initial(p,0)==-VOS_EBADHANDLE,"zero grants create no ambient authority");
    file_close_all(p);
}

static void concurrent(void)
{
    struct process *p=&procs[0],*q=&procs[1];
    expect(!grant(p,"dup/x",FILE_RIGHTS) && !grant(q,"dup/y",FILE_RIGHTS),"concurrent grants");
    worker_handle[0]=file_initial(p,0);worker_handle[1]=file_initial(q,0);
    blk_invalidate(device);md.hold=1;
    int64_t a=thread_create("file-first",worker,(void *)0);
    int wa=blocked(a);
    int64_t b=thread_create("file-next",worker,(void *)1);
    int wb=blocked(b);
    expect(wa && wb && io_scope_count()==2 && !file_audit(),"two processes retain independent scopes while reader serializes");
    struct ext2_stat st;
    expect(file_stat(p,worker_handle[0],&st)==-VOS_EAGAIN && file_close(p,worker_handle[0])==-VOS_EAGAIN &&
           proc_start(p)==PROC_ESTATE && file_volume_detach()==-VOS_EAGAIN,"active session cannot be reused, closed, started or detached");
    file_close_all(p);
    struct ext2_stat poison;memset(&poison,0xa5,sizeof(poison));
    expect(gone(a) && gone(b) && worker_result[0]==-FILE_EINTR && worker_result[1]==0 &&
           !memcmp(&worker_stat[0],&poison,sizeof(poison)) && !io_scope_count() && !file_process_busy(p),"cancelled owner unwinds; waiting reader succeeds; no output or ticket leak");
    file_close_all(q);
    expect(!file_session_count() && !file_handle_count() && !file_audit() && !ext2_audit() && !blk_audit(),"concurrent cleanup audited");
    expect(!grant(p,"dup",FILE_RIGHTS),"reset waiter grant");worker_handle[0]=file_initial(p,0);
    blk_invalidate(device);md.hold=1;
    a=thread_create("file-reset",worker,NULL);wa=blocked(a);
    uint64_t reads=md.reads;blk_note_reset(device);thread_wake_all(&gate);
    expect(wa && gone(a) && worker_result[0]==-FILE_ESTALE && md.reads==reads && !io_scope_count(),"reset observed immediately after wait; no further backend read");
    file_close_all(p);expect(!file_volume_detach() && !file_volume_attach(p,device),"reset-waiter recovery");
}

static void capacity_and_processes(void)
{
    for (unsigned i=0;i<PROC_TABLE;i++) {
        expect(!grant(&procs[i],"dup",FILE_RIGHTS),"capacity grant");
        uint64_t base=file_initial(&procs[i],0);
        for (unsigned j=1;j<FILE_MAX;j++) expect(open_path(&procs[i],base,".",0)>0,"fill embedded nodes");
        expect(open_path(&procs[i],base,".",0)==-VOS_ENOMEM,"full per-process table refuses without cross-process spill");
    }
    expect(file_handle_count()==64 && file_session_count()==8 && !file_audit() && !io_scope_count(),"global node capacity is exactly partitioned 64");
    expect(proc_begin(&procs[PROC_TABLE],"ninth")==PROC_EBUSY,"ninth process/session cannot exceed pool");
    expect(proc_run(&procs[0])==PROC_EXITED,"real process exit releases all file handles");
    expect(!proc_start(&procs[1]),"start kill owner");proc_kill(&procs[1],"file lifecycle test");
    expect(proc_wait(&procs[1])==PROC_KILLED,"real process kill releases grants and derived nodes");
    expect(proc_run(&procs[2])==PROC_KILLED,"user fault releases files");
    expect(file_handle_count()==40 && file_session_count()==5 && !proc_audit(),"terminated owners have no file resources");
    uint64_t old=file_initial(&procs[3],0);
    for (unsigned i=0;i<PROC_TABLE;i++) proc_destroy(&procs[i]);
    expect(!file_handle_count() && !file_session_count() && !io_scope_count() && !file_audit(),"destroy never-started and terminated owners releases all resources");
    expect(!proc_create(&procs[0],"reused",exit_code,sizeof(exit_code)) && !grant(&procs[0],"dup",FILE_RIGHTS),"process address reused with new PID");
    struct ext2_stat st;
    expect(file_stat(&procs[0],old,&st)==-VOS_EBADHANDLE,"process-slot reuse cannot inherit old token");
    file_close_all(&procs[0]);
}

static void cancelled_setup(void)
{
    struct process *p=&procs[0];
    expect(!grant(p,"dup",FILE_RIGHTS),"open cancellation setup");worker_handle[0]=file_initial(p,0);
    for (unsigned op=1;op<=3;op++) {
        if (op==3) expect(!file_volume_detach(),"cancelled attach starts detached");
        worker_operation=op;blk_invalidate(device);md.hold=1;
        int64_t tid=thread_create("file-unwind",worker,NULL);int waiting=blocked(tid);
        expect(waiting && file_process_busy(p) && proc_start(p)==PROC_ESTATE,"pending open/provision/attach retains process owner");
        file_close_all(p);
        expect(gone(tid) && worker_result[0]==-FILE_EINTR && !file_session_count() &&
               !file_handle_count() && !file_process_busy(p) && !io_scope_count() && !file_audit(),
               "cancelled open reservation/provision/attach fully unwinds");
    }
    worker_operation=0;
    expect(!ext2_open_count() && !file_volume_attach(p,device),"cancelled mount reader reclaimed and reusable");
}

static void virtio(void)
{
    struct process *p=&procs[0];const struct vblk_info *vi=vblk_info();
    expect(!file_volume_detach(),"module volume detached before real transport");
    if (vi->state!=VBLK_READY) {serial_puts("file: real virtio not applicable (no ready device)\r\n");return;}
    expect(!file_volume_attach(p,vi->blk_id) && !grant(p,"dup/x",FILE_RIGHTS),"real virtio directory provisioning");
    uint64_t h=file_initial(p,0);int64_t f=open_path(p,h,"name.txt",FILE_READ);
    expect(f>0 && file_read_at(p,f,buffer,4096,0)>0 && !file_close(p,f),"real transport read in scoped file layer");
    blk_invalidate(vi->blk_id);
    if (vi->mode==VBLK_MODE_IRQ) {
        vblk_test_wait_ticks(12);vblk_test_skip_notify(1);worker_handle[0]=h;
        int64_t tid=thread_create("file-abandon",worker,NULL);
        int waiting=blocked(tid);file_close_all(p);
        expect(waiting && gone(tid) && worker_result[0]==-FILE_EINTR && !file_session_count() &&
               io_scope_count()==1 && vblk_abandoned()==1 && !file_audit() && !vblk_audit(),"real cancellation releases session; scalar debt survives reaped stack");
        expect(file_volume_detach()==-VOS_EAGAIN,"detached DMA debt prevents remount");
        thread_sleep(16);
        expect(!io_scope_count() && !vblk_abandoned() && !file_handle_count(),"orphan retires without future file calls");
        vblk_test_wait_ticks(0);
    } else {
        vblk_test_poll_limit(2000);vblk_test_skip_notify(1);struct ext2_stat st;
        expect(file_stat(p,h,&st)==-FILE_ETIMEDOUT && !io_scope_count() && file_stat(p,h,&st)==-FILE_ESTALE,"poll timeout wins for initiator, then stale");
        vblk_test_poll_limit(0);file_close_all(p);
    }
    expect(!file_volume_detach() && vblk_reinit()==VBLK_READY && !vblk_audit(),"real transport teardown/recovery");
}

static void token_exhaustion(void)
{
    struct process *p=&procs[0];
    expect(!file_volume_attach(p,device) && !grant(p,"dup",FILE_RIGHTS),"serial tail setup");
    uint64_t base=file_initial(p,0);
    file_test_last_tokens();             /* never moved backward/reused, even in tests */
    int64_t a=open_path(p,base,".",0),b=open_path(p,base,".",0);
    expect(a==(int64_t)(FILE_TOKEN_DOMAIN|(FILE_SERIAL_MAX-1)) && b==INT64_MAX &&
           open_path(p,base,".",0)==-VOS_ENOMEM && !io_scope_count() && !file_audit(),"actual opens reach last two tokens then roll back cleanly on exhaustion");
    expect(!file_close(p,a) && open_path(p,base,".",0)==-VOS_ENOMEM && file_close(p,a)==-VOS_EBADHANDLE,"free slot cannot reset exhausted token issuer");
    file_close_all(p);expect(!file_volume_detach(),"serial exhaustion leaves no live node");
}

static void fixture_tests(const struct boot_info *bi)
{
    const struct boot_module *m=NULL;
    for (unsigned i=0;i<bi->module_count;i++)
        if (strlen(bi->modules[i].cmdline)==11 && !memcmp(bi->modules[i].cmdline,"ext2-1k.img",11)) m=&bi->modules[i];
    if (!m) {expect(0,"required fixture missing");return;}
    uint64_t frames=pmm_free_count();unsigned threads=thread_count(),devices=blk_count();
    memset(&md,0,sizeof(md));md.img=phys_to_virt(m->start);md.bytes=m->end-m->start;md.epoch=1;md.state=BLK_DEV_READY;
    expect(!blk_register("file-fixture",&ops,NULL,&device),"module backend registered");
    for (unsigned i=0;i<PROC_TABLE;i++)
        expect(!proc_create(&procs[i],"file-test",i==2?fault_code:exit_code,i==2?sizeof(fault_code):sizeof(exit_code)),"registered process owner");
    basic();paths();failures_and_cookies();concurrent();cancelled_setup();capacity_and_processes();virtio();token_exhaustion();
    proc_destroy(&procs[0]);
    expect(!file_volume_detach() && !blk_unregister(device) && !file_session_count() && !file_handle_count() &&
           !io_scope_count() && !ext2_open_count() && !proc_count() && !file_audit() && !proc_audit() &&
           !sched_audit() && !blk_audit() && !ext2_audit() && pmm_free_count()==frames &&
           thread_count()==threads && blk_count()==devices,"exact final frame/thread/device/process/reader/session/ticket accounting");
}
#endif

int selftest_file(const struct boot_info *bi)
{
    cases=failures=0;
    uint64_t serial=FILE_SERIAL_MAX-1;
    expect(file_test_issue(&serial)==(FILE_TOKEN_DOMAIN| (FILE_SERIAL_MAX-1)) &&
           file_test_issue(&serial)==INT64_MAX && !file_test_issue(&serial) && serial==FILE_TOKEN_DOMAIN,
           "last two serials followed by permanent exhaustion");
    serial=0;expect(!file_test_issue(&serial),"zero serial refused");
    expect(file_volume_attach(NULL,0)==-VOS_EINVAL && file_initial(NULL,0)==-VOS_EBADHANDLE &&
           file_initial((struct process *)1,0)==-VOS_EBADHANDLE &&
           file_close(NULL,1)==-VOS_EBADHANDLE && !file_audit(),"invalid owners and clean baseline");
#ifdef VOS_TEST_CASE_ext2
    fixture_tests(bi);
#else
    (void)bi;serial_puts("file: fixture tests not applicable (no ext2 test modules)\r\n");
#endif
    serial_puts("selftest: file ");serial_dec(cases);serial_puts(" cases, ");serial_dec(failures);serial_puts(" failed\r\n");
    return failures;
}

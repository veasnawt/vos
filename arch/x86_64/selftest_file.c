/* I2 kernel and I3 ring-3 tests. Original ext2 fixture bytes stay immutable: faults
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
#include "loader.h"
#include "exception.h"
#include "usercopy.h"

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
    uint64_t fail_sector, prefix_sector;
    unsigned prefix_seen, failure_seen;
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
    if (md.fail_sector && lba<md.fail_sector+8 && lba+count>md.fail_sector) {
        md.failure_seen++;
        return BLK_EIO;
    }
    if (md.prefix_sector && lba<=md.prefix_sector && lba+count>md.prefix_sector)
        md.prefix_seen++;
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

static void rights_width(void)
{
    struct process *p=&procs[0], *q=&procs[3];
    expect(!grant(p,"dup",FILE_RIGHTS),"full-width rights setup");
    uint64_t h=file_initial(p,0);
    int64_t before=file_open_at(p,h,".",1,0);
    expect(before>0 && !file_close(p,before),"rights rejection token baseline");
    unsigned handles=file_handle_count(), sessions=file_session_count();
    uint64_t reads=md.reads;
    /* Includes the original three reproductions: bit32, bit63, READ|bit32;
     * all undefined low bits and every high bit are checked in both paths.
     */
    for(unsigned bit=4;bit<64;bit++) {
        uint64_t bad=UINT64_C(1)<<bit;
        for(unsigned mix=0;mix<3;mix++) {
            uint64_t rights=bad | (mix==1 ? FILE_READ : mix==2 ? FILE_RIGHTS : 0);
            struct file_grant g={0,rights,"dup",3};
            expect(file_open_at(p,h,"x/name.txt",10,rights)==-VOS_EINVAL &&
                   file_grants_install(q,&g,1)==-VOS_EINVAL &&
                   file_initial(q,0)==-VOS_EBADHANDLE && md.reads==reads &&
                   file_handle_count()==handles && file_session_count()==sessions &&
                   !io_scope_count() && !file_audit(),"undefined rights rejected before I/O/publication/session reservation");
        }
    }
    struct file_grant gs[2]={{0,FILE_RIGHTS,"dup",3},{1,UINT64_MAX,"deep",4}};
    expect(file_grants_install(q,gs,2)==-VOS_EINVAL && md.reads==reads &&
           file_session_count()==sessions && file_handle_count()==handles,
           "invalid second grant has no partial setup effects");
    int64_t after=file_open_at(p,h,".",1,0);
    expect(after==before+1 && !file_close(p,after),"rejected rights consume no token serials");
    for(uint64_t rights=0;rights<=FILE_RIGHTS;rights++) {
        struct file_grant g={0,rights,"dup",3};
        expect(!file_grants_install(q,&g,1),"every defined grant combination accepted");
        int64_t token=file_open_at(p,h,"x/name.txt",10,rights);
        struct ext2_stat st;
        expect(token>0 && file_stat(p,token,&st)==(rights&FILE_STAT?0:-VOS_EACCES) &&
               file_read_at(p,token,NULL,0,0)==(rights&FILE_READ?0:-VOS_EACCES) &&
               !file_close(p,token),"every defined file combination retains exact READ/STAT authority");
        uint64_t parent=file_initial(q,0);
        for(uint64_t child_rights=0;child_rights<=FILE_RIGHTS;child_rights++) {
            token=file_open_at(q,parent,".",1,child_rights);
            int allowed=(rights&FILE_TRAVERSE) && !(child_rights & ~rights);
            expect(allowed ? token>0 && !file_close(q,token) : token==-VOS_EACCES,
                   "all defined rights subsets preserve independent traversal and attenuation");
        }
        file_close_all(q);
    }
    file_close_all(p);
    expect(!file_handle_count() && !file_session_count() && !io_scope_count() &&
           !file_audit() && !blk_audit() && !ext2_audit() && !proc_audit(),
           "full-width rights matrix leaves exact empty ownership state");
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
    expect(!file_volume_attach(p,device) && !grant(p,"dup",FILE_RIGHTS),"legacy ID rollover setup");h=file_initial(p,0);
    struct blk_desc before,after;blk_describe(device,&before);oldid=device;
    expect(!blk_unregister(device) && !blk_test_reuse_id(oldid) &&
           !blk_register("file-fixture",&ops,NULL,&device) && device==oldid,"simulate same numeric device ID without observing removal");
    blk_describe(device,&after);uint64_t reads=md.reads;
    expect(before.incarnation!=after.incarnation && before.epoch==after.epoch && before.reset_seq==after.reset_seq &&
           file_stat(p,h,&st)==-FILE_ESTALE && reads==md.reads,"never-reused incarnation rejects aliased device ID before any I/O");
    file_close_all(p);expect(!file_volume_detach(),"aliased registry binding teardown");
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

/* I3: separately linked real ring-3 callers; reuse I2 processes/backend.
 * Never accept a userspace log line as a verdict. The kernel checks exit,
 * exact trap/counters, observed blocked states and final ownership audits.
 */
static int load_file_program(const struct boot_info *bi, unsigned index, const char *name)
{
    struct elf_image image; int error;
    for (unsigned i=0;i<bi->module_count;i++) {
        const struct boot_module *m=&bi->modules[i];
        if (strlen(m->cmdline)==strlen(name) && !memcmp(m->cmdline,name,strlen(name)))
            return load_elf(&procs[index],name,phys_to_virt(m->start),m->end-m->start,&image,&error)==PROC_OK;
    }
    return 0;
}
static int exited(struct process *p)
{
    return proc_wait(p)==PROC_EXITED && p->exit_status==0;
}
static int error_result(struct process *p, int error)
{
    return proc_wait(p)==PROC_EXITED && p->exit_status==error;
}
static int pause_program(struct process *p, struct process *controller, int64_t *peer, uint64_t completed)
{
    int64_t handle;
    return !channel_create(p,HANDLE_RECV,controller,HANDLE_SEND,&handle,peer) && handle==1 &&
           !proc_start(p) && blocked(p->tid) && p->syscalls==completed && !file_process_busy(p);
}
static void release_program(struct process *controller, int64_t peer)
{
    expect(!channel_send(controller,peer,0,0) && !handle_close(controller,peer),
           "explicit zero-byte channel barrier releases exactly one prepared reader");
}
/* I4: independently built callers. Control data is copied through the same
 * ordinary channel/usercopy boundary used by applications, never into a
 * fixture's file table or return frame. Snapshots cover each launch. */
static int start_adversary(struct process *p, struct process *controller,
                           uint64_t mode, uint64_t foreign)
{
    int64_t handle,peer;
    uint64_t command[2]={mode,foreign};
    uint64_t address=USER_STACK_END-sizeof(command);
    return !channel_create(p,HANDLE_RECV,controller,HANDLE_SEND,&handle,&peer) && handle==1 &&
           !copy_to_user(controller->root,address,command,sizeof(command)) &&
           !channel_send(controller,peer,address,sizeof(command)) && !handle_close(controller,peer) &&
           !proc_start(p);
}
static void token_exhaustion(const struct boot_info *bi)
{
    struct process *p=&procs[0], *q=&procs[1];
    proc_destroy(p);
    expect(load_file_program(bi,0,"file-adversary") &&
           !proc_create(q,"tail-control",exit_code,sizeof(exit_code)),"I4 ring3 serial-tail owners");
    expect(!file_volume_attach(q,device) && !grant(p,"dup",FILE_RIGHTS) &&
           !grant(q,"dup",FILE_RIGHTS),"serial tail grants precede irreversible jump");
    uint64_t base=file_initial(q,0);
    file_test_last_tokens(); /* never moved backward/reused, even in tests */
    expect(start_adversary(p,q,5,0) && exited(p),"I4 actual syscalls consume last two serials then permanently fail without wrap/reuse");
    expect(open_path(q,base,".",0)==-VOS_ENOMEM && !io_scope_count() && !file_audit(),
           "exhausted global issuer cannot be revived by another process or freed slot");
    proc_destroy(q);expect(!file_volume_detach(),"serial exhaustion leaves no live node");
}
static void adversarial(const struct boot_info *bi, struct process *controller)
{
    struct process *p=&procs[0], *q=&procs[1];
    uint64_t frames=pmm_free_count();
    unsigned threads=thread_count(), readers=ext2_open_count();
    uint32_t dup=child(2,"dup"),x=child(dup,"x"),file=child(x,"name.txt");
    uint64_t entry=dir_entry(x,"name.txt"),inode=inode_pos(file);
    expect(!proc_create(q,"foreign-i4",exit_code,sizeof(exit_code)) && !grant(q,"dup/y",FILE_RIGHTS),
           "I4 independent foreign owner");
    expect(load_file_program(bi,0,"file-adversary") && !grant(p,"dup",FILE_RIGHTS) &&
           start_adversary(p,controller,0,file_initial(q,0)),"I4 ring3 adversarial launch");
    expect(exited(p) && p->syscalls>4000,"I4 full-width rights, all subsets, token confusion, memory and 32 quota/reuse cycles");
    proc_destroy(p);proc_destroy(q);
    expect(!file_handle_count() && !file_session_count() && !io_scope_count() && !proc_audit() &&
           pmm_free_count()==frames && thread_count()==threads,"I4 matrix exact frame/thread/handle/session reclamation");
    /* Each mutation is private to this RAM backend; original disk bytes and
     * the real virtio image remain untouched. Include a reserved inode,
     * not just an out-of-range inode, to exercise parser/API error mapping. */
    for(unsigned mutation=0;mutation<12;mutation++) {
        expect(load_file_program(bi,0,"file-adversary") && !grant(p,"dup/x",FILE_RIGHTS),"I4 corrupt-metadata grant before immutable overlay");
        uint8_t bytes[4]={0};
        switch(mutation) {
        case 0: patch(entry+4,bytes,2); break;                 /* zero rec_len */
        case 1: bytes[0]=10;patch(entry+4,bytes,2);break;      /* unaligned */
        case 2: bytes[0]=4;bytes[1]=4;patch(entry+4,bytes,2);break; /* overrun */
        case 3: bytes[0]='/';patch(entry+8,bytes,1);break;
        case 4: patch(entry+8,bytes,1);break;                  /* embedded NUL */
        case 5: bytes[0]=8;patch(entry+7,bytes,1);break;       /* unknown kind */
        case 6: memset(bytes,0xff,4);patch(entry,bytes,4);break;
        case 7: bytes[0]=3;patch(entry,bytes,4);break;         /* reserved inode */
        case 8: patch(inode+26,bytes,2);break;                /* no links */
        case 9: patch(inode,bytes,2);break;                   /* invalid mode */
        case 10: bytes[0]=1;patch(inode+28,bytes,4);break;    /* invalid blocks512 */
        case 11: bytes[0]=EXT2_T_DIR;patch(entry+7,bytes,1);break; /* type mismatch */
        }
        blk_invalidate(device);
        int ok=start_adversary(p,controller,1,0) && exited(p);
        if(!ok) {
            serial_puts("I4 metadata mutation ");serial_dec(mutation);
            serial_puts(" exit ");serial_dec(p->exit_status);serial_puts("\r\n");
        }
        expect(ok,"I4 corrupt metadata returns EIO with atomic output and reusable reader");
        proc_destroy(p);md.npatch=0;blk_invalidate(device);
        expect(!file_handle_count() && !file_session_count() && !io_scope_count() && !file_audit() &&
               !ext2_audit() && !blk_audit() && pmm_free_count()==frames && thread_count()==threads,
               "I4 each rejected corruption releases exact resources");
    }
    for(unsigned mode=2;mode<=4;mode++) {
        expect(load_file_program(bi,0,"file-adversary") && !grant(p,mode==2?"dup":"dup/x",FILE_RIGHTS),"I4 bounded walk/exact-byte setup");
        if(mode==2) alias(dup,"x",dup,EXT2_T_DIR);
        else if(mode==3) {
            const uint8_t name[]={0xc3,0xa9,'.','t','x','t'},n=sizeof(name);
            patch(entry+6,&n,1);patch(entry+8,name,sizeof(name));blk_invalidate(device);
        } else {
            const uint8_t name[]={0xff,'a','m','e','.','t','x','t'};
            patch(entry+8,name,sizeof(name));blk_invalidate(device);
        }
        expect(start_adversary(p,controller,mode,0) && exited(p),"I4 finite cyclic walk and no Unicode decoding/normalization in ring3");
        proc_destroy(p);md.npatch=0;blk_invalidate(device);
    }
    for(unsigned round=0;round<3;round++) for(unsigned mode=6;mode<=(round?8u:11u);mode++) {
        expect(load_file_program(bi,0,"file-adversary") && !grant(p,"dup/x",FILE_RIGHTS),"I4 repeated syscall-kill setup");
        blk_invalidate(device);md.hold=1;
        expect(start_adversary(p,controller,mode,0) && blocked(p->tid) && file_process_busy(p),"I4 observe open/stat/enumeration blocked inside disk I/O");
        if(mode<9) {
            proc_kill(p,"I4 pending operation kill");
            unsigned call=mode==6?SYS_FILE_OPEN_AT:mode==7?SYS_FILE_STAT:SYS_DIRECTORY_NEXT;
            expect(proc_wait(p)==PROC_KILLED && p->trap.rax==call && !file_process_busy(p),"I4 killed operation unwinds without returning or publishing");
        } else {
            expect(thread_interrupt(p->tid)==1 && exited(p),"I4 nonfatal open/stat/enumeration interruption is atomic and retry succeeds");
        }
        proc_destroy(p);
        expect(!file_session_count() && !file_handle_count() && !io_scope_count() && !proc_audit() &&
               !ext2_audit() && !blk_audit() && pmm_free_count()==frames && thread_count()==threads &&
               ext2_open_count()==readers,"I4 repeated kills release exact owners, reservations, references, pages and stacks");
    }
}
static void file_userspace(const struct boot_info *bi)
{
    struct process *p=&procs[0],*q=&procs[1],*controller=&procs[7];
    expect(!file_volume_attach(p,device),"module volume for userspace API tests");
    proc_destroy(p);
    expect(!proc_create(controller,"file-control",exit_code,sizeof(exit_code)),"trusted IPC barrier controller");
    expect(load_file_program(bi,0,"file-probe") && p->abi==2,"real ABI v2 ELF negotiation");
    expect(!grant(p,"dup",FILE_RIGHTS) &&
           !proc_create(q,"foreign",exit_code,sizeof(exit_code)) && !grant(q,"dup/y",FILE_RIGHTS),
           "explicit launch grants and immediately following foreign token");
    int64_t channel,peer;
    expect(!channel_create(p,HANDLE_RIGHTS,q,HANDLE_RIGHTS,&channel,&peer) && channel==1,
           "ABI v2 retains ordinary channel close on the disjoint domain");
    expect(!proc_start(p) && exited(p) && p->syscalls>70 && p->sys_efault>=10,
           "ring3 file contracts, rights, paths, pointers, wire bytes, cookies, close and quota");
    proc_destroy(p);proc_destroy(q);
    adversarial(bi,controller);
    const char *gates[]={"file-gate0","file-gate1"};
    for(unsigned i=0;i<2;i++) {
        expect(load_file_program(bi,0,gates[i]) && p->abi==i && !proc_start(p) && exited(p) &&
               p->sys_enosys==(i?5u:6u),"real old ABI rejects new calls before invalid arguments; close gate preserved");
        proc_destroy(p);
    }
    expect(load_file_program(bi,0,"file-nogrant") && !proc_start(p) && exited(p),
           "ABI v2 launch without trusted grants has no ambient filesystem authority");
    proc_destroy(p);
    uint32_t dup=child(2,"dup"), x=child(dup,"x");
    uint32_t targets[]={child(2,"link-short"),2,child(2,"pipe")};
    uint8_t kinds[]={EXT2_T_LNK,EXT2_T_DIR,EXT2_T_FIFO};
    int errors[]={FILE_ELOOP,VOS_EACCES,FILE_EUNSUPP};
    for(unsigned i=0;i<3;i++) {
        expect(load_file_program(bi,0,"file-boundary") && !grant(p,"dup",FILE_RIGHTS),"ring3 boundary overlay setup");
        alias(dup,"x",targets[i],kinds[i]);
        expect(!proc_start(p) && proc_wait(p)==PROC_EXITED && p->exit_status==errors[i],
               "ring3 final/intermediate symlink, root alias and special-node confinement");
        proc_destroy(p);md.npatch=0;blk_invalidate(device);
    }
    expect(load_file_program(bi,0,"file-cookie") && !grant(p,"dup/x",FILE_RIGHTS),"ring3 malformed enumeration setup");
    const uint8_t zero[2]={0,0};patch(dir_entry(x,"name.txt")+4,zero,2);blk_invalidate(device);
    expect(!proc_start(p) && exited(p),"ring3 malformed entry preserves every directory output byte");
    proc_destroy(p);md.npatch=0;blk_invalidate(device);
    expect(load_file_program(bi,0,"file-regs") && !grant(p,"dup",FILE_RIGHTS) &&
           pause_program(p,controller,&peer,1),"register caller blocked at barrier before stat");
    blk_invalidate(device);md.hold=1;release_program(controller,peer);
    expect(blocked(p->tid) && file_process_busy(p),"register stat actually blocks on I/O");
    thread_wake_all(&gate);
    expect(exited(p),"all non-result registers, RFLAGS, stack and red zone preserved across blocking file syscall");
    proc_destroy(p);
    /* Force two distinct cache lines in a large file. The first ext2 data
     * chunk succeeds; all backend/fallback reads of the next fail. */
    expect(load_file_program(bi,0,"file-error") && !grant(p,"dup/x",FILE_RIGHTS),"partial private-prefix failure setup");
    uint32_t data=child(2,"data.bin");
    alias(x,"name.txt",data,EXT2_T_REG);
    uint64_t ip=inode_pos(data), first_sector=(uint64_t)u32(ip+40)*2;
    uint32_t distant=u32(ip+40+8*4);
    uint8_t pb[4]={(uint8_t)distant,(uint8_t)(distant>>8),(uint8_t)(distant>>16),(uint8_t)(distant>>24)};
    patch(ip+44,pb,4);blk_invalidate(device);
    expect(pause_program(p,controller,&peer,2),"prefix caller blocked at barrier after opening large file");
    blk_invalidate(device);md.prefix_sector=first_sector;md.fail_sector=((uint64_t)distant*2)&~UINT64_C(7);
    md.prefix_seen=md.failure_seen=0;
    release_program(controller,peer);
    expect(error_result(p,FILE_EIO) && md.prefix_seen && md.failure_seen,
           "I/O error after a successful private data chunk leaves all 4096 user bytes unchanged");
    proc_destroy(p);md.fail_sector=md.prefix_sector=0;md.npatch=0;blk_invalidate(device);
    /* Both callers block at explicit barriers after open, before read. */
    expect(load_file_program(bi,0,"file-reader") && !grant(p,"dup/x",FILE_RIGHTS) &&
           load_file_program(bi,1,"file-reader-y") && !grant(q,"dup/y",FILE_RIGHTS),"real concurrent reader provisioning");
    struct process *third=&procs[2];int64_t third_peer;
    expect(load_file_program(bi,2,"file-error") && !grant(third,"dup/x",FILE_RIGHTS) &&
           pause_program(third,controller,&third_peer,2),"I4 third caller provisioned before ticket exhaustion");
    int64_t second_peer;
    expect(pause_program(p,controller,&peer,2) && pause_program(q,controller,&second_peer,2),
           "both ring3 readers blocked at barriers after open");
    blk_invalidate(device);md.hold=1;
    release_program(controller,peer);release_program(controller,second_peer);
    expect(blocked(p->tid) && blocked(q->tid) && file_process_busy(p) && file_process_busy(q) &&
           io_scope_count()==2 && !file_audit(),"real file syscalls blocked concurrently with private scopes");
    uint64_t reads_before=md.reads;
    release_program(controller,third_peer);
    expect(error_result(third,VOS_EAGAIN) && io_scope_count()==2 && md.reads==reads_before &&
           file_process_busy(p) && file_process_busy(q) && !file_audit(),
           "I4 third ring3 caller fails admission without I/O/output or stealing either ticket");
    proc_destroy(third);
    thread_wake_all(&gate);
    expect(exited(p) && exited(q),"both ring3 readers resume, read repeatedly and exit");
    proc_destroy(p);proc_destroy(q);
    expect(load_file_program(bi,0,"file-reader-y") && !grant(p,"dup/x",FILE_RIGHTS) &&
           pause_program(p,controller,&peer,2),"negative control: Y reader receives X grant");
    release_program(controller,peer);
    expect(error_result(p,4),"reader fixture rejects the other process's distinct contents");
    proc_destroy(p);
    expect(!file_session_count() && !file_handle_count() && !io_scope_count() && !proc_audit(),
           "ring3 exit reclaims all nodes and scopes");
    /* Actual kill inside read_at, not a pre-entry kill or kernel-only caller. */
    expect(load_file_program(bi,0,"file-reader") && !grant(p,"dup/x",FILE_RIGHTS) &&
           pause_program(p,controller,&peer,2),"kill caller blocked at barrier before read");
    blk_invalidate(device);md.hold=1;release_program(controller,peer);
    expect(blocked(p->tid) && file_process_busy(p),"observe actual pending userspace I/O before kill");
    proc_kill(p,"file syscall kill");
    expect(proc_wait(p)==PROC_KILLED && p->trap.rax==SYS_FILE_READ_AT && p->syscalls==3 &&
           !file_handle_count() && !file_session_count() && !io_scope_count(),"killed file syscall unwinds before process cleanup and never returns");
    proc_destroy(p);
    /* Recoverable error, changed binding and nonfatal interruption all leave
     * the caller's complete 4096-byte sentinel unchanged (checked in ring3). */
    for(unsigned scenario=0;scenario<5;scenario++) {
        expect(load_file_program(bi,0,"file-error") && !grant(p,"dup/x",FILE_RIGHTS) &&
               pause_program(p,controller,&peer,2),"error caller blocked at barrier before read");
        blk_invalidate(device);
        if(!scenario) md.fail=1;
        else if(scenario==3) expect(!blk_unregister(device),"registry removal before pending userspace read");
        else if(scenario==4) md.state=BLK_DEV_UNAVAILABLE;
        else md.hold=1;
        release_program(controller,peer);
        if(scenario==1 || scenario==2) {
            expect(blocked(p->tid) && file_process_busy(p),"error injection observes pending read");
            if(scenario==1) {blk_note_reset(device);thread_wake_all(&gate);}
            else expect(thread_interrupt(p->tid)==1,"nonfatal interruption delivered to observed blocked reader");
        }
        int error=scenario==0?FILE_EIO:scenario==1 || scenario==3?FILE_ESTALE:scenario==4?FILE_EUNAVAIL:FILE_EINTR;
        expect(error_result(p,error),
               "exact negative userspace read result preserves entire output");
        md.fail=0;md.state=BLK_DEV_READY;proc_destroy(p);
        if(scenario==1 || scenario==3) {
            expect(!file_volume_detach(),"retired binding has no active ownership");
            if(scenario==3) expect(!blk_register("file-fixture",&ops,NULL,&device),"explicit registration after removal");
            expect(!proc_create(p,"setup",exit_code,sizeof(exit_code)) &&
                   !file_volume_attach(p,device),"explicit remount after reset");
            proc_destroy(p);
        }
    }
    /* Real driver abandonment with a process's trap/stack/pages live until
     * unwind; afterwards only scalar ticket debt survives, serviced by idle. */
    expect(!file_volume_detach(),"detach module volume before ring3 transport test");
    const struct vblk_info *vi=vblk_info();
    if(vi->state==VBLK_READY) {
        expect(load_file_program(bi,0,"file-reader") && !file_volume_attach(p,vi->blk_id) &&
               !grant(p,"dup/x",FILE_RIGHTS) && pause_program(p,controller,&peer,2),"transport caller blocked at barrier before read");
        blk_invalidate(vi->blk_id);vblk_test_skip_notify(1);
        if(vi->mode==VBLK_MODE_IRQ) {
            vblk_test_wait_ticks(12);release_program(controller,peer);
            expect(blocked(p->tid) && file_process_busy(p),"real virtio IRQ syscall is pending");
            proc_kill(p,"file DMA kill");
            expect(proc_wait(p)==PROC_KILLED && p->trap.rax==SYS_FILE_READ_AT &&
                   !file_session_count() && io_scope_count()==1 && vblk_abandoned()==1,"real ring3 kill leaves only scalar driver debt");
            proc_destroy(p);
            expect(file_volume_detach()==-VOS_EAGAIN,"debt gates teardown after user pages freed");
            thread_sleep(16);vblk_test_wait_ticks(0);
            expect(!io_scope_count() && !vblk_abandoned() && !vblk_audit(),"orphan independently retires after caller destruction");
        } else {
            vblk_test_poll_limit(8192);vblk_test_yield_polls(32);
            uint64_t yields=vi->yields;
            release_program(controller,peer);
            for(unsigned wait=0;wait<100 && (!file_process_busy(p) || vi->yields==yields);wait++) thread_yield();
            expect(file_process_busy(p) && thread_state(p->tid)==THREAD_RUNNABLE && vi->yields>yields,
                   "actual userspace read pending at known driver poll-yield point");
            proc_kill(p,"file polling kill");
            expect(proc_wait(p)==PROC_KILLED && p->trap.rax==SYS_FILE_READ_AT &&
                   !file_session_count() && !io_scope_count() && !vblk_abandoned(),
                   "polling kill unwinds at finite request bound with no user return or debt");
            proc_destroy(p);vblk_test_poll_limit(0);vblk_test_yield_polls(4096);
            expect(!file_volume_detach() && vblk_reinit()==VBLK_READY,"polling kill resets and drains before new launch");
            expect(load_file_program(bi,0,"file-error") && !file_volume_attach(p,vi->blk_id) &&
                   !grant(p,"dup/x",FILE_RIGHTS) && pause_program(p,controller,&peer,2),"timeout caller blocked at barrier before read");
            blk_invalidate(vi->blk_id);vblk_test_skip_notify(1);vblk_test_poll_limit(2000);
            release_program(controller,peer);
            expect(error_result(p,FILE_ETIMEDOUT),"real userspace polling timeout has no output prefix");
            vblk_test_poll_limit(0);proc_destroy(p);
        }
        expect(!file_volume_detach() && vblk_reinit()==VBLK_READY && !vblk_audit(),"ring3 transport fully drained and recovered");
    }
    proc_destroy(controller);
    expect(!proc_create(p,"tail",exit_code,sizeof(exit_code)),"restore internal owner before irreversible token-tail test");
    expect(!file_session_count() && !file_handle_count() && !io_scope_count() && !proc_audit() &&
           !sched_audit() && !ext2_audit() && !blk_audit(),"all userspace I/O ownership audits clean");
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
    basic();rights_width();paths();failures_and_cookies();concurrent();cancelled_setup();capacity_and_processes();virtio();
    file_userspace(bi);token_exhaustion(bi);
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
    serial=UINT64_MAX-1;
    expect(blk_test_issue_incarnation(&serial)==UINT64_MAX-1 && !blk_test_issue_incarnation(&serial) &&
           serial==UINT64_MAX,"device incarnation issuer saturates without reuse");
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

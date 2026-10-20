#include <vos/file.h>

/* The trusted runner selects immutable overlays and sends only scalar test
 * inputs over an ordinary ABI v1 channel. Failures are kernel-checked exits. */
#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
static unsigned char output[4096];
static char path[1025];
static const char protected[] = "read only";
static void poison(void) { for (unsigned i=0;i<sizeof(output);i++) output[i]=0xa5; }
static int intact(void) {
    for (unsigned i=0;i<sizeof(output);i++) if (output[i]!=0xa5) return 0;
    return 1;
}
static int closed(uint64_t h) {
    return vos_file_stat(h,output,64)==-VOS_EBADHANDLE &&
           vos_file_read_at(h,0,0,0)==-VOS_EBADHANDLE &&
           vos_directory_next(h,0,output,272)==-VOS_EBADHANDLE &&
           vos_file_open_at(h,".",1,0)==-VOS_EBADHANDLE &&
           vos_handle_close(h)==-VOS_EBADHANDLE;
}
int main(void)
{
    uint64_t command[2];
    REQUIRE(vos_channel_recv(1,command,sizeof(command))==sizeof(command));
    REQUIRE(vos_handle_close(1)==0);
    long h=vos_file_initial(0);
    REQUIRE(h>0);
    if (command[0]==5) {
        long a=vos_file_open_at(h,".",1,0),b=vos_file_open_at(h,".",1,0);
        REQUIRE(a==INT64_MAX-1 && b==INT64_MAX);
        REQUIRE(vos_file_open_at(h,".",1,0)==-VOS_ENOMEM);
        REQUIRE(vos_handle_close(a)==0 && closed(a));
        REQUIRE(vos_file_open_at(h,".",1,0)==-VOS_ENOMEM);
        REQUIRE(vos_handle_close(b)==0 && closed(b));
        REQUIRE(vos_file_initial(0)==h);
        return 0;
    }
    if (command[0]>=6 && command[0]<=11) {
        poison();
        unsigned operation=(command[0]-6)%3;
        long r=operation==0?vos_file_open_at(h,"name.txt",8,VOS_FILE_READ):
               operation==1?vos_file_stat(h,output,64):vos_directory_next(h,0,output,272);
        if (command[0]<9) return 99; /* must be killed inside the operation */
        REQUIRE(r==-VOS_EINTR && intact());
        r=operation==0?vos_file_open_at(h,"name.txt",8,VOS_FILE_READ):
          operation==1?vos_file_stat(h,output,64):vos_directory_next(h,0,output,272);
        REQUIRE(operation==0?r>0 && vos_handle_close(r)==0:operation==1?r==0:r==1);
        return 0;
    }
    if (command[0]==1) {
        /* Directory parser or target-inode corruption: neither an open nor
         * enumeration may publish a prefix, token or private inode number. */
        poison();
        REQUIRE(vos_file_open_at(h,"name.txt",8,0)==-VOS_EIO);
        REQUIRE(vos_directory_next(h,0,output,272)==-VOS_EIO && intact());
        REQUIRE(vos_file_stat(h,output,64)==0); /* reader reusable after error */
        return 0;
    }
    if (command[0]==2) {
        /* A cyclic child graph with 64 legal components must hit the finite
         * work budget rather than restart its budget or hang. */
        for(unsigned i=0;i<64;i++) { path[2*i]='x'; path[2*i+1]='/'; }
        REQUIRE(vos_file_open_at(h,path,127,0)==-VOS_EAGAIN);
        REQUIRE(vos_file_stat(h,output,64)==0);
        return 0;
    }
    if (command[0]==3) {
        const char nfc[]={ (char)0xc3,(char)0xa9,'.','t','x','t' };
        const char nfd[]={ 'e',(char)0xcc,(char)0x81,'.','t','x','t' };
        long f=vos_file_open_at(h,nfc,sizeof(nfc),0);
        REQUIRE(f>0 && vos_file_open_at(h,nfd,sizeof(nfd),0)==-VOS_ENOENT);
        REQUIRE(vos_handle_close(f)==0);
        return 0;
    }
    if (command[0]==4) {
        const char raw[]={ (char)0xff,'a','m','e','.','t','x','t' };
        long f=vos_file_open_at(h,raw,sizeof(raw),0);
        REQUIRE(f>0 && vos_file_open_at(h,"name.txt",8,0)==-VOS_ENOENT);
        REQUIRE(vos_handle_close(f)==0);
        return 0;
    }
    REQUIRE(command[0]==0);
    poison();
    REQUIRE(closed(command[1]) && intact()); /* actual foreign live token */
    REQUIRE(closed(0) && closed(UINT64_MAX) && closed(UINT64_C(1)<<62));
    for(unsigned bit=0;bit<64;bit++) {
        REQUIRE(closed(UINT64_C(1)<<bit));
        REQUIRE(closed((uint64_t)h^(UINT64_C(1)<<bit)));
    }
    REQUIRE(vos_file_initial(UINT64_C(1)<<32)==-VOS_EINVAL);
    REQUIRE(vos_file_initial(UINT64_MAX)==-VOS_EINVAL);
    REQUIRE(vos_channel_recv(h,0,0)==-VOS_EBADHANDLE);
    long before=vos_file_open_at(h,".",1,0);
    REQUIRE(before>0 && vos_handle_close(before)==0);
    for(unsigned bit=4;bit<64;bit++)
        for(unsigned mask=0;mask<16;mask++)
            REQUIRE(vos_file_open_at(h,".",1,(UINT64_C(1)<<bit)|mask)==-VOS_EINVAL);
    long after=vos_file_open_at(h,".",1,0);
    REQUIRE(after==before+1 && vos_handle_close(after)==0);
    for(uint64_t rights=0;rights<16;rights++) {
        long parent=vos_file_open_at(h,"x",1,rights);
        REQUIRE(parent>0);
        for(uint64_t child=0;child<16;child++) {
            long derived=vos_file_open_at(parent,".",1,child);
            if (!(rights&VOS_FILE_TRAVERSE) || (child&~rights)) {
                REQUIRE(derived==-VOS_EACCES);
            } else {
                REQUIRE(derived>0);
                REQUIRE(vos_file_stat(derived,output,64)==(child&VOS_FILE_STAT?0:-VOS_EACCES));
                REQUIRE(vos_directory_next(derived,0,output,272)==(child&VOS_FILE_ENUMERATE?1:-VOS_EACCES));
                REQUIRE(vos_handle_close(derived)==0 && closed(derived));
            }
        }
        REQUIRE(vos_handle_close(parent)==0);
    }
    long directory=vos_file_open_at(h,"x",1,VOS_FILE_ENUMERATE);
    REQUIRE(directory>0);
    /* Independent 1 KiB fixture geometry: dot at 0, dotdot at 12, name at
     * 24, and the final record ends at 1024. Probe every forged byte offset. */
    for(uint64_t cookie=0;cookie<=1025;cookie++) {
        poison();
        long r=vos_directory_next(directory,cookie,output,272);
        long expected=cookie==0 || cookie==12 || cookie==24?1:cookie==1024?0:-VOS_EINVAL;
        REQUIRE(r==expected);
        if (r!=1) REQUIRE(intact());
        else REQUIRE(output[0]==0 && output[1]==4 && output[8]==1 && output[12]==8);
    }
    REQUIRE(vos_handle_close(directory)==0);
    for(unsigned i=0;i<sizeof(path);i++) path[i]='x';
    REQUIRE(vos_file_open_at(h,path,256,0)==-VOS_ENAMETOOLONG);
    REQUIRE(vos_file_open_at(h,path,1024,0)==-VOS_ENAMETOOLONG);
    for(unsigned i=0;i<65;i++) { path[2*i]='.'; path[2*i+1]='/'; }
    REQUIRE(vos_file_open_at(h,path,129,0)==-VOS_EINVAL);
    long dots=vos_file_open_at(h,path,127,0);
    REQUIRE(dots>0 && vos_handle_close(dots)==0);
    const char *invalid[]={"../x","x/../y","x/../../..","./../x","x/..",".."};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        unsigned n=0;while(invalid[i][n]) n++;
        REQUIRE(vos_file_open_at(h,invalid[i],n,0)==-VOS_EACCES);
    }
    long f=vos_file_open_at(h,"x/name.txt",10,VOS_FILE_READ|VOS_FILE_STAT);
    REQUIRE(f>0);
    const uint64_t bad[]={0,1,0x40000000,UINT64_C(0x800000000000),
        UINT64_C(0xffff800000000000),UINT64_C(0xffffffff80000000),UINT64_MAX-15,
        (uint64_t)protected,(uint64_t)main,UINT64_C(0x7fffffffeffc)};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        REQUIRE(vos_file_read_at(f,(void *)bad[i],64,0)==-VOS_EFAULT);
        REQUIRE(vos_file_stat(f,(void *)bad[i],64)==-VOS_EFAULT);
        REQUIRE(vos_directory_next(h,0,(void *)bad[i],272)==-VOS_EFAULT);
    }
    REQUIRE(vos_file_read_at(f,output,UINT64_C(1)<<32,0)==-VOS_EINVAL);
    REQUIRE(vos_file_read_at(f,output,1,UINT64_MAX)==-VOS_EINVAL);
    REQUIRE(vos_file_read_at(f,0,0,UINT64_MAX)==0);
    REQUIRE(vos_handle_close(f)==0);
    /* Repeated slot reuse, quota refusal, every operation, and stale-token
     * rejection. Filling all seven derived slots proves rejected calls did
     * not strand reservations. The runner audits exact resources per launch. */
    for(unsigned round=0;round<32;round++) {
        long slots[7];
        for(unsigned i=0;i<7;i++) {
            slots[i]=vos_file_open_at(h,"x/name.txt",10,VOS_FILE_READ|VOS_FILE_STAT);
            REQUIRE(slots[i]>0);
        }
        REQUIRE(vos_file_open_at(h,".",1,0)==-VOS_ENOMEM);
        for(unsigned i=0;i<7;i++) {
            REQUIRE(vos_file_stat(slots[i],output,64)==0);
            REQUIRE(vos_file_read_at(slots[i],output,4096,0)==19);
            REQUIRE(vos_handle_close(slots[i])==0 && closed(slots[i]));
        }
        REQUIRE(vos_directory_next(h,0,output,272)==1);
        REQUIRE(vos_yield()==0);
    }
    return 0;
}

/* Gate A only. No SC01--SC11 verdicts. Each failure has a distinct exit code. */
#include <vos/file.h>

static unsigned char buffer[4096];
static void fill(void) { for (unsigned i=0;i<sizeof(buffer);i++) buffer[i]=0xa5; }
static int untouched(unsigned start) {
    for (unsigned i=start;i<sizeof(buffer);i++) if(buffer[i]!=0xa5) return 0;
    return 1;
}
static int same(const void *want, unsigned n) {
    const unsigned char *p=want;
    for(unsigned i=0;i<n;i++) if(buffer[i]!=p[i]) return 0;
    return 1;
}
static int data(unsigned role,unsigned region) {
    for(unsigned j=0;j<4096;j++) {
        unsigned char want=((0x4E360001U >> (8*(j&3)))+73*role+29*region+17*j+(j>>8))&255;
        if(buffer[j]!=want) return 0;
    }
    return 1;
}
int main(void)
{
    long base=vos_file_initial(0);
    if(base<=0) return 1;
    long f=vos_file_open_at(base,"r0/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
    if(f<=0) return 2;
    fill();
    if(vos_file_read_at(f,buffer,4096,0)!=4096 || !data(0,0)) return 3;
    struct vos_file_stat want={.size=69632,.blocks512=138,.kind=1,.mode=0100444,
                               .uid=0,.gid=0,.links=1,.rights=3};
    fill();
    if(vos_file_stat(f,buffer,64)!=0 || !same(&want,64) || !untouched(64)) return 4;
    if(vos_handle_close(f)) return 5;
    f=vos_file_open_at(base,"r1/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
    if(f<=0) return 6;
    fill();
    if(vos_file_read_at(f,buffer,4096,16*4096)!=4096 || !data(1,16)) return 7;
    if(vos_handle_close(f)) return 8;
    long d=vos_file_open_at(base,"canary",6,VOS_FILE_STAT|VOS_FILE_ENUMERATE);
    if(d<=0) return 9;
    struct vos_directory_entry entry={.next_cookie=1024,.kind=1,.name_length=4,
                                      .name={'o','n','l','y'}};
    fill();
    if(vos_directory_next(d,0,buffer,272)!=1 || !same(&entry,272) || !untouched(272)) return 10;
    fill();
    if(vos_directory_next(d,1024,buffer,272)!=0 || !untouched(0)) return 11;
    if(vos_handle_close(d)) return 12;
    if(vos_handle_close(base)) return 13;
    return 0;
}

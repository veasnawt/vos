/* SC02: trusted test setup fills scalars at an existing zero-length debug call. */
#include <vos/file.h>
#include <stdint.h>
static unsigned char bytes[4096];
static struct { uint64_t index,term,old,foreign,buffer; } command;
static int pattern(void) {
    for(unsigned j=0;j<4096;j++) {
        unsigned char expected=((0x4e360001U>>(8*(j&3)))+17*j+(j>>8))&255;
        if(bytes[j]!=expected) return 0;
    }
    return 1;
}
int main(void) {
    command.buffer=(uintptr_t)bytes;
    if(vos_debug_write(&command,0)) return 1;
    long base=vos_file_initial(command.index);
    if(base<=0 || vos_file_initial(2)!=-VOS_EINVAL) return 2;
    long f=vos_file_open_at(base,"r0/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
    if(f<=0 || vos_file_read_at(f,bytes,4096,0)!=4096 || !pattern()) return 3;
    if(vos_file_stat(f,bytes,64)) return 4;
    struct vos_file_stat want={.size=69632,.blocks512=138,.kind=1,.mode=0100444,.links=1,.rights=3};
    for(unsigned i=0;i<64;i++) if(bytes[i]!=((unsigned char *)&want)[i]) return 5;
    long d=vos_file_open_at(base,"canary",6,VOS_FILE_ENUMERATE|VOS_FILE_STAT);
    if(d<=0 || vos_directory_next(d,0,bytes,272)!=1) return 6;
    struct vos_directory_entry ent={.next_cookie=1024,.kind=1,.name_length=4,.name={'o','n','l','y'}};
    for(unsigned i=0;i<272;i++) if(bytes[i]!=((unsigned char *)&ent)[i]) return 7;
    if(vos_handle_close(f) || vos_handle_close(d)) return 8;
    if(vos_file_read_at(f,bytes,4096,0)!=-VOS_EBADHANDLE ||
       vos_file_read_at(command.old,bytes,4096,0)!=-VOS_EBADHANDLE ||
       vos_file_read_at(command.foreign,bytes,4096,0)!=-VOS_EBADHANDLE) return 9;
    long handles[6];
    for(unsigned i=0;i<6;i++) if((handles[i]=vos_file_open_at(base,".",1,VOS_FILE_STAT))<=0) return 10;
    if(vos_file_open_at(base,".",1,VOS_FILE_STAT)!=-VOS_ENOMEM) return 11;
    for(unsigned i=0;i<6;i++) if(vos_handle_close(handles[i])) return 12;
    /* Leave real grants and derived handles for exit/fault/kill cleanup. */
    if(vos_file_open_at(base,"r0/data.bin",11,VOS_FILE_READ)<=0 ||
       vos_file_open_at(base,"canary",6,VOS_FILE_ENUMERATE)<=0) return 13;
    if(vos_debug_write(0,0)) return 14;
    if(command.term==1) __asm__ volatile("ud2");
    if(command.term==2) for(;;) __asm__ volatile("pause");
    return 0;
}

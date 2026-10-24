/* Gate C: one file operation per attempt, each behind a zero-length debug barrier.
 * Trusted test setup fills the scalars at the first debug call; the kernel
 * controller verifies every result and every destination byte at the barrier
 * that follows each attempt, so the sentinel is refilled only after that barrier.
 */
#include <vos/file.h>
#include <stdint.h>
static unsigned char bytes[4096];
static struct { uint64_t buffer,op,attempts,region,stale; } command;
int main(void) {
    command.buffer=(uintptr_t)bytes;
    if(vos_debug_write(&command,0)) return 1;
    long base=vos_file_initial(0);
    if(base<=0) return 2;
    /* A killed incarnation's token confers nothing in this one. */
    if(command.stale && vos_file_stat((long)command.stale,bytes,64)!=-VOS_EBADHANDLE) return 3;
    long h=base;
    if(command.op==1 || command.op==2) h=vos_file_open_at(base,"r0/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
    if(command.op==4) h=vos_file_open_at(base,"r1/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
    if(command.op==3) h=vos_file_open_at(base,"canary",6,VOS_FILE_ENUMERATE|VOS_FILE_STAT);
    if(h<=0) return 4;
    for(uint64_t a=0;a<command.attempts;a++) {
        if(vos_debug_write(0,0)) return 5;
        for(unsigned j=0;j<4096;j++) bytes[j]=0xa5;
        if(command.op==0) (void)vos_file_open_at(base,"r0/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
        else if(command.op==1 || command.op==4) (void)vos_file_read_at(h,bytes,4096,command.region*4096);
        else if(command.op==2) (void)vos_file_stat(h,bytes,64);
        else (void)vos_directory_next(h,0,bytes,272);
    }
    if(vos_debug_write(0,0)) return 6;
    return 0;
}

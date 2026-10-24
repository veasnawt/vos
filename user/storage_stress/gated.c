/* Gate D: one path or directory operation per attempt, each behind a zero-length debug barrier.
 * Trusted test setup fills the scalars and the path at each barrier; the kernel controller verifies
 * every result, every cost and every destination byte at the barrier that follows each attempt, and
 * each enumeration call as it returns. The sentinel is refilled only after a barrier and before
 * every enumeration call, so each inspection sees its own call's destination.
 */
#include <vos/file.h>
#include <stdint.h>
static unsigned char bytes[4096];
static char path[1024];
static struct { uint64_t buffer,path,op,attempts,len,cookie,mode,reserved; } command;
static void fill(void) { for(unsigned j=0;j<4096;j++) bytes[j]=0xa5; }
int main(void) {
    command.buffer=(uintptr_t)bytes;
    command.path=(uintptr_t)path;
    if(vos_debug_write(&command,0)) return 1;
    long scope=vos_file_initial(0),many=vos_file_initial(1);
    if(scope<=0 || many<=0) return 2;
    long dir=vos_file_open_at(many,".",1,VOS_FILE_ENUMERATE|VOS_FILE_STAT);
    long huge=vos_file_open_at(scope,"huge",4,VOS_FILE_ENUMERATE|VOS_FILE_STAT);
    long file=vos_file_open_at(scope,"r0/data.bin",11,VOS_FILE_READ|VOS_FILE_STAT);
    long closed=vos_file_open_at(many,".",1,VOS_FILE_ENUMERATE);
    if(dir<=0 || huge<=0 || file<=0 || closed<=0 || vos_handle_close(closed)) return 3;
    for(uint64_t a=0;a<command.attempts;a++) {
        if(vos_debug_write(0,0)) return 4;
        fill();
        if(command.op==0) (void)vos_file_open_at(scope,path,command.len,VOS_FILE_ENUMERATE|VOS_FILE_STAT);
        else if(command.op==1) {
            if(command.mode==1) {                   /* close the directory handle and enumerate a reopened one */
                if(vos_handle_close(dir)) return 5;
                if((dir=vos_file_open_at(many,".",1,VOS_FILE_ENUMERATE|VOS_FILE_STAT))<=0) return 6;
            }
            uint64_t cookie=0;
            for(unsigned n=0;n<=200;n++) {          /* at most 200 entries and EOF */
                fill();
                if(vos_directory_next(dir,cookie,bytes,272)!=1) break;
                cookie=0;
                for(unsigned j=0;j<8;j++) cookie|=(uint64_t)bytes[j]<<(8*j);
            }
        }
        else if(command.op==2) (void)vos_directory_next(huge,command.cookie,bytes,272);
        else if(command.mode==0) (void)vos_directory_next(closed,0,bytes,272);
        else if(command.mode==1) (void)vos_directory_next(file,0,bytes,272);
        else (void)vos_directory_next(dir,0,bytes,271);
    }
    if(vos_debug_write(0,0)) return 7;
    return 0;
}

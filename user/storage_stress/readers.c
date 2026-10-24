/* SC01: one separately built ABI v2 reader per role. No trusted verdict here. */
#include <vos/file.h>
#ifndef READER_ROLE
#error missing role
#endif
static unsigned char bytes[4096];
static int pattern(unsigned region) {
    for(unsigned j=0;j<4096;j++) {
        unsigned char expected=((0x4e360001U>>(8*(j&3)))+73*READER_ROLE+29*region+17*j+(j>>8))&255;
        if(bytes[j]!=expected) return 0;
    }
    return 1;
}
int main(void) {
    if(vos_debug_write(bytes,0)) return 1;
    long base=vos_file_initial(0);
    long f=vos_file_open_at(base,"data.bin",8,VOS_FILE_READ|VOS_FILE_STAT);
    if(base<=0 || f<=0) return 2;
    for(unsigned e=0;e<68;e++) {
        for(unsigned j=0;j<4096;j++) bytes[j]=0xa5;
        if(vos_channel_recv(1,0,0)) return 3;
        unsigned region=(5*e)%17;
        if(vos_file_read_at(f,bytes,4096,region*4096)!=4096 || !pattern(region)) return 4;
        if(vos_channel_send(1,0,0)) return 5;
    }
    if(vos_handle_close(f) || vos_handle_close(base) || vos_handle_close(1)) return 6;
    return 0;
}

#include <vos/file.h>
static unsigned char buffer[4096];
int main(void)
{
    long h=vos_file_initial(0);
    long f=vos_file_open_at(h,"name.txt",8,VOS_FILE_READ|VOS_FILE_STAT);
    if(f<0) return 1;
    for(unsigned i=0;i<sizeof(buffer);i++) buffer[i]=0xa5;
    /* Explicit ABI v1 barrier: the controller arms I/O before sending a
     * zero-byte message. A single yield cannot establish this ordering. */
    if(vos_channel_recv(1,0,0)<0) return 7;
    long r=vos_file_read_at(f,buffer,sizeof(buffer),0);
#ifdef EXPECT_ERROR
    if(r!=-VOS_EIO && r!=-VOS_ESTALE && r!=-VOS_ETIMEDOUT && r!=-VOS_EINTR && r!=-VOS_EUNAVAIL && r!=-VOS_EAGAIN) return 2;
    for(unsigned i=0;i<sizeof(buffer);i++) if(buffer[i]!=0xa5) return 3;
    return (int)-r; /* trusted runner checks the exact error as well as intact bytes */
#else
#ifdef EXPECT_Y
    const char *expected="name.txt in /dup/y (a different file)\n";
#else
    const char *expected="name.txt in /dup/x\n";
#endif
    unsigned size=0;while(expected[size]) size++;
    if(r!=size || buffer[size]!=0xa5) return 4;
    for(unsigned i=0;i<size;i++) if(buffer[i]!=(unsigned char)expected[i]) return 6;
    for(unsigned i=0;i<8;i++) if(vos_file_read_at(f,buffer,sizeof(buffer),0)!=size) return 5;
    return 0; /* process exit closes both still-live handles */
#endif
}

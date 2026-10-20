#include <vos/file.h>
static unsigned char output[272];
int main(void)
{
    long h=vos_file_initial(0);
    for(unsigned i=0;i<sizeof(output);i++) output[i]=0xa5;
    if(vos_directory_next(h,0,output,sizeof(output))!=-VOS_EIO) return 1;
    for(unsigned i=0;i<sizeof(output);i++) if(output[i]!=0xa5) return 2;
    return 0;
}

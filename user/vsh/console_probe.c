#include <vos/syscall.h>
#include <vos/console.h>
#ifndef EXPECT_GATE
static const char readonly[]="immutable";
#endif
int main(void)
{
#ifdef EXPECT_GATE
    if(vos_console_read((void*)0,0)!=-EXPECT_GATE) return 1;
    if(vos_console_write((void*)0,0)!=-EXPECT_GATE) return 2;
    if(vos_console_clear()!=-EXPECT_GATE) return 3;
#else
    unsigned char data[132];for(unsigned i=0;i<sizeof(data);i++)data[i]=0x5A;
    if(vos_console_read(data,0)!=-VOS_EINVAL || vos_console_read(data,130)!=-VOS_EINVAL) return 4;
    if(vos_console_read((void*)0,129)!=-VOS_EFAULT || vos_console_read((void*)~0UL,129)!=-VOS_EFAULT) return 5;
    if(vos_console_read((void*)readonly,8)!=-VOS_EFAULT) return 6;
    if(vos_console_write((void*)0,257)!=-VOS_EINVAL || vos_console_write((void*)0,1)!=-VOS_EFAULT) return 7;
    if(vos_console_write((void*)0,0)!=0) return 8;
    if(vos_console_clear()!=0) return 9;
    vos_debug_write("probe: ready",12);
    if(vos_console_read(data,2)!=-VOS_EMSGSIZE) return 10;
    for(unsigned i=0;i<sizeof(data);i++)if(data[i]!=0x5A)return 11;
    if(vos_console_read(data,4)!=3 || data[0]!='a'||data[1]!='b'||data[2]!='c'||data[3]||data[4]!=0x5A) return 12;
    vos_debug_write("probe: empty",12);
    if(vos_console_read(data+8,1)!=0 || data[8] || data[9]!=0x5A) return 13;
#endif
    return 0;
}

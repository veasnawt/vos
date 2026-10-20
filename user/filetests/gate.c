#include <vos/file.h>
int main(void)
{
    if(vos_file_open_at(-1,(void *)-1,-1,-1)!=-VOS_ENOSYS ||
       vos_file_read_at(-1,(void *)-1,-1,-1)!=-VOS_ENOSYS ||
       vos_file_stat(-1,(void *)-1,-1)!=-VOS_ENOSYS ||
       vos_directory_next(-1,-1,(void *)-1,-1)!=-VOS_ENOSYS ||
       vos_file_initial(-1)!=-VOS_ENOSYS) return 1;
    return vos_handle_close(0x4000000000000001UL)==
           -(TEST_ABI ? VOS_EBADHANDLE : VOS_ENOSYS) ? 0 : 2;
}

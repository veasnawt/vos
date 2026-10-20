#include <vos/file.h>
int main(void)
{
    return vos_file_initial(0)==-VOS_EBADHANDLE && vos_file_initial(1)==-VOS_EBADHANDLE &&
           vos_file_initial(2)==-VOS_EINVAL &&
           vos_file_open_at(0x4000000000000001UL,".",1,VOS_FILE_RIGHTS)==-VOS_EBADHANDLE ? 0 : 1;
}

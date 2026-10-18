/*
 * N2.4 (ABI v1): handle 1 has the receive right only. Sending must be
 * refused (-VOS_EACCES); closing works once; the closed handle is dead.
 * Exit 0, or the number of the first failed check.
 */
#include "fixture.h"

int main(void)
{
    char c = 'x';

    if (vos_channel_send(1, &c, 1) != -VOS_EACCES)
        return 1;
    if (vos_yield() != 0)
        return 2;
    if (vos_handle_close(1) != 0)
        return 3;
    if (vos_handle_close(1) != -VOS_EBADHANDLE || vos_channel_recv(1, &c, 1) != -VOS_EBADHANDLE)
        return 4;
    return 0;
}

/*
 * N2.4 (ABI v1): send numbered 8-byte messages on handle 1 until the queue
 * is full (-VOS_EAGAIN), then close the handle. Exit with the number
 * accepted, or 200 + a check number.
 */
#include "fixture.h"

int main(void)
{
    unsigned long msg[1];
    int n = 0;

    for (;;) {
        msg[0] = 0x464C4F4F44000000UL + (unsigned long)n;
        long r = vos_channel_send(1, msg, sizeof(msg));
        if (r == -VOS_EAGAIN)
            break;
        if (r != 0 || n > 100)
            return 201;
        n++;
    }
    if (vos_handle_close(1) != 0 || vos_handle_close(1) != -VOS_EBADHANDLE ||
        vos_channel_send(1, msg, 8) != -VOS_EBADHANDLE)
        return 202;
    return n;
}

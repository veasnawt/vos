/*
 * N2.4 (ABI v1): receive on handle 1 until the peer is gone, checking that
 * the numbered messages of ipc_flood arrive in order. Exit with the count,
 * or 200 + a check number.
 */
#include "fixture.h"

int main(void)
{
    unsigned long msg[1];
    int n = 0;

    for (;;) {
        long r = vos_channel_recv(1, msg, sizeof(msg));
        if (r == -VOS_EPIPE)
            return n;
        if (r != 8)
            return 201;
        if (msg[0] != 0x464C4F4F44000000UL + (unsigned long)n)
            return 202;
        n++;
    }
}

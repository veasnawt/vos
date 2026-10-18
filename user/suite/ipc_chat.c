/*
 * N2.5 (ABI v1): talk forever on handle 1: send a numbered 8-byte message,
 * wait for its echo, check it, repeat. It only ends when the peer is gone:
 * exit 100 if a send found the peer closed, 101 if a receive did, or 1-3 on
 * a wrong result (so a peer dying in the middle of a conversation is
 * observable).
 */
#include "fixture.h"

int main(void)
{
    unsigned long n = 0, buf[4];

    for (;;) {
        buf[0] = 0x4348415400000000UL + n;
        long s = vos_channel_send(1, buf, 8);
        if (s == -VOS_EPIPE)
            return 100;
        if (s != 0)
            return 1;
        long r = vos_channel_recv(1, buf, sizeof(buf));
        if (r == -VOS_EPIPE)
            return 101;
        if (r != 8)
            return 2;
        if (buf[0] != 0x4348415400000000UL + n)
            return 3;
        n++;
    }
}

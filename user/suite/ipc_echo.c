/*
 * N2.4 (ABI v1): receive on handle 1 and send every message back unchanged,
 * until the peer is gone (-VOS_EPIPE). Exit with the number echoed, or 200 +
 * a check number on an unexpected result.
 */
#include "fixture.h"

static unsigned char buf[VOS_MSG_MAX];

int main(void)
{
    int n = 0;

    for (;;) {
        long r = vos_channel_recv(1, buf, sizeof(buf));
        if (r == -VOS_EPIPE)
            return n;
        if (r < 0)
            return 201;
        long s = vos_channel_send(1, buf, (unsigned long)r);
        if (s == -VOS_EPIPE)
            return n;
        if (s != 0)
            return 202;
        n++;
    }
}

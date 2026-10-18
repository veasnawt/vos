/*
 * N2.4 (ABI v1): block in channel_recv on handle 1 for a message that never
 * comes; only the kernel ends this program (it is killed while blocked).
 */
#include "fixture.h"

static unsigned char buf[VOS_MSG_MAX];

int main(void)
{
    long r = vos_channel_recv(1, buf, sizeof(buf));
    return r == -VOS_EPIPE ? 77 : 78;
}

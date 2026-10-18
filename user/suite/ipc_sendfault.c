/*
 * N2.5 (ABI v1): send three numbered 8-byte messages (the ipc_flood format)
 * on handle 1, then fault with a null read. The messages already queued must
 * still reach the peer, and nothing of the channel may be damaged.
 */
#include "fixture.h"

int main(void)
{
    unsigned long msg[1];

    for (int n = 0; n < 3; n++) {
        msg[0] = 0x464C4F4F44000000UL + (unsigned long)n;
        if (vos_channel_send(1, msg, sizeof(msg)) != 0)
            return 1;
    }
    (void)*(volatile int *)0;
    not_reached();
}

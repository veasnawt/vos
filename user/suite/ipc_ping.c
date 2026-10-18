/*
 * N2.4 (ABI v1): the client side of the ping/echo scenarios, on handle 1.
 * Sends PING_N messages of different lengths (0..256), checking error
 * behaviour after the first one, then receives the echoes and compares them
 * byte for byte. Exit 0 if everything matched, 100 if the peer was already
 * gone on the first send (-VOS_EPIPE comes before buffer checks), else the
 * number of the first failed check. The kernel judges
 * the run by its own records; this status is one input.
 */
#include "fixture.h"

#define PING_N 10

static unsigned char out[VOS_MSG_MAX + 1], in[VOS_MSG_MAX];

static unsigned len_of(int i)
{
    return i == 0 ? 0 : i == 1 ? VOS_MSG_MAX : (unsigned)(i * 23);
}

static void fill(int i)
{
    for (unsigned k = 0; k < len_of(i); k++)
        out[k] = (unsigned char)(i * 31 + k);
}

static int error_checks(void)
{
    if (vos_channel_send(1, out, VOS_MSG_MAX + 1) != -VOS_EINVAL)
        return 1;
    if (vos_channel_send(2, out, 1) != -VOS_EBADHANDLE || vos_channel_send(0, out, 1) != -VOS_EBADHANDLE)
        return 2;
    if (vos_channel_send(1, (const void *)0x10, 4) != -VOS_EFAULT)
        return 3;
    if (vos_channel_send(1, (const void *)0xFFFFFFFF80100000UL, 4) != -VOS_EFAULT)
        return 4;
    return 0;
}

int main(void)
{
    for (int i = 0; i < PING_N; i++) {
        fill(i);
        long r = vos_channel_send(1, out, len_of(i));
        if (i == 0 && r == -VOS_EPIPE)
            return 100;
        if (r != 0)
            return 5;
        if (i == 0 && error_checks())
            return error_checks();
    }
    for (int i = 0; i < PING_N; i++) {
        fill(i);
        if (i == 1) {                   /* the 256-byte echo: too small a buffer first, then a bad one */
            if (vos_channel_recv(1, in, VOS_MSG_MAX - 1) != -VOS_EMSGSIZE)
                return 6;
            if (vos_channel_recv(1, (void *)0x400000, VOS_MSG_MAX) != -VOS_EFAULT)
                return 7;
        }
        long r = vos_channel_recv(1, in, sizeof(in));
        if (r != (long)len_of(i))
            return 8;
        for (unsigned k = 0; k < len_of(i); k++)
            if (in[k] != out[k])
                return 9;
    }
    return 0;
}

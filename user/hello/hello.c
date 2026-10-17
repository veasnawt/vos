/* The first separately built VOS application. */
#include <vos/syscall.h>

static const char message[] = "Hello from VOS!\n";

int main(void)
{
    long n = vos_debug_write(message, sizeof(message) - 1);

    return n == (long)(sizeof(message) - 1) ? 0 : 1;
}

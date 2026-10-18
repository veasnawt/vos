#include "blockdev.h"

const char *blk_strerror(int err)
{
    static const char *const s[] = {
        "ok", "no device", "invalid request", "sectors outside the device", "device I/O error",
        "request not supported by the device", "timed out", "all request slots busy",
        "device reset, reinitialization needed", "device broke the queue protocol",
    };

    return err >= 0 && err <= BLK_EPROTO ? s[err] : "unknown error";
}

#include "storage_observe.h"
#ifdef VOS_TEST
#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "pmm.h"

int storage_observe(struct storage_observation *out)
{
    if (!out || irq_in_handler() || (read_rflags() & RFLAGS_IF))
        return -1;
    memset(out, 0, sizeof(*out));
    proc_test_observe(out->processes);
    thread_test_observe(out->threads);
    file_test_observe(&out->file);
    ext2_test_observe(out->readers);
    blk_test_observe(&out->block);
    io_scope_test_observe(out->tickets);
    vblk_test_observe(&out->driver);
    out->pmm_free = pmm_kernel()->free;
    out->pmm_usable = pmm_kernel()->usable;
    return 0;
}
#endif

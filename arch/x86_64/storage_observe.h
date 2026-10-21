/* N3.6 I2: bounded read-only observations of authoritative state.
 * Test-only, kernel-private, trusted output buffers, thread context, IF=0.
 * No allocation, callback, wait, device-register access or state transition.
 */
#ifndef VOS_X86_64_STORAGE_OBSERVE_H
#define VOS_X86_64_STORAGE_OBSERVE_H
#ifdef VOS_TEST
#include "bootinfo.h"
#include "file.h"
#include "io_scope.h"
#include "virtio_blk.h"

struct observe_process {
    uint64_t record, pid, root, tid, stack_top;
    unsigned state, pages;
};
struct observe_thread {
    uint64_t record, tid, process, rsp, lo, hi, stack_owner, queue, next;
    int state, stack;
};
struct observe_node {
    uint64_t token, generation;
    unsigned ino, rights, kind, used;
};
struct observe_session {
    uint64_t owner, pid, tid, initial[FILE_INITIAL_MAX];
    unsigned active, closing, provisioned;
    struct observe_node nodes[FILE_MAX];
};
struct observe_file {
    uint64_t fs, generation, incarnation, epoch, reset_seq, sectors;
    uint64_t config_owner, config_pid, config_tid;
    unsigned device, refs, active, dead;
    struct observe_session sessions[PROC_TABLE];
};
struct observe_reader {
    uint64_t record, epoch, head, tail;
    unsigned used, busy, dead, device;
};
struct observe_cache_line {
    uint64_t frame, epoch, number, checksum;
    unsigned state, slot, generation, sectors, pins;
};
struct observe_block {
    struct { uint64_t incarnation, reset_seq; unsigned used, id; } devices[BLK_MAX_DEVICES];
    struct observe_cache_line lines[BLK_CACHE_LINES];
    uint64_t head, tail;
    unsigned ready, devices_count;
    struct blk_stats stats;
};
struct observe_ticket {
    uint64_t id, pid, tid, scope, guard, deadline;
    unsigned request, abandoned, logical, reads, waits;
    int result;
};
struct observe_slot {
    uint64_t owner, tid, ticket, head, tail, abandoned_at;
    unsigned inflight, done, abandoned, count, length;
};
struct observe_driver {
    struct observe_slot slots[VBLK_SLOTS];
    struct { uint64_t address; unsigned length; uint16_t flags, next; } descriptors[VBLK_QUEUE_SIZE];
    uint64_t pool[VBLK_POOL_FRAMES];
    uint64_t submitted, completed, cancelled, retired_completion, retired_reset, resets;
    unsigned state, mode, pool_held, quarantined, next_avail, last_used;
    unsigned descriptors_available; /* initialized, allocated CPU-readable table; not request ownership */
};
struct storage_observation {
    struct observe_process processes[PROC_TABLE];
    struct observe_thread threads[THREAD_MAX];
    struct observe_file file;
    struct observe_reader readers[EXT2_MAX_FS];
    struct observe_block block;
    struct observe_ticket tickets[IO_SCOPE_TICKETS];
    struct observe_driver driver;
    uint64_t pmm_free, pmm_usable;
};
_Static_assert(sizeof(struct storage_observation) <= 16384, "bounded observation");
void proc_test_observe(struct observe_process out[PROC_TABLE]);
void thread_test_observe(struct observe_thread out[THREAD_MAX]);
void file_test_observe(struct observe_file *out);
void ext2_test_observe(struct observe_reader out[EXT2_MAX_FS]);
void blk_test_observe(struct observe_block *out);
void io_scope_test_observe(struct observe_ticket out[IO_SCOPE_TICKETS]);
void vblk_test_observe(struct observe_driver *out);
/* Synthetic observation-input filter only; never changes the real queue/device. */
void vblk_test_observe_hide_table(int hide);
/* Returns 0; rejects IRQ/IF=1 callers without modifying output. */
int storage_observe(struct storage_observation *out);
int selftest_accounting(const struct boot_info *bi);
#endif
#endif

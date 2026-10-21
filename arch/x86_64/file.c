#include "file.h"
#include "io_scope.h"
#include "lib.h"
#include "panic.h"
#include "syscall.h"
#include "exception.h"
#include "usercopy.h"

struct node {
    uint64_t token, generation;
    uint32_t ino;
    unsigned rights, kind, used;         /* 0 free, 1 reserved, 2 published */
};
struct session {
    struct process *owner;
    uint64_t pid, tid, initial[FILE_INITIAL_MAX];
    unsigned active, closing, provisioned;
    struct node nodes[FILE_MAX];
    char path[EXT2_PATH_MAX + 1];
    uint8_t stage[4096];
};
static struct session sessions[PROC_TABLE];
static struct {
    struct ext2_fs *fs;
    blk_id_t device;
    uint64_t epoch, reset_seq, sectors, generation, incarnation;
    unsigned refs, active, dead;
    struct process *config_owner;
    uint64_t config_pid, config_tid;
} volume;
static uint64_t next_token = 1, next_volume = 1;
_Static_assert(sizeof(struct node) <= 64, "node budget");
_Static_assert(sizeof(sessions) + sizeof(volume) <= 60 * 1024, "file static budget incl I1 headroom");

static uint64_t issue(uint64_t *serial)
{
    if (!*serial || *serial > FILE_SERIAL_MAX)
        return 0;
    return FILE_TOKEN_DOMAIN | (*serial)++;
}

/* Registered pointer first; do not dereference a forged process pointer. */
static int eligible(const struct process *p)
{
    if (!proc_file_live(p))
        return 0;
    struct thread *t = thread_current();
    return t && (p->state == PROC_READY || (t->proc == p && t->id == p->tid));
}

static struct session *find(const struct process *p)
{
    for (unsigned i = 0; p && i < PROC_TABLE; i++)
        if (sessions[i].owner == p && sessions[i].pid == p->pid)
            return &sessions[i];
    return NULL;
}

static struct session *allocate(struct process *p)
{
    for (unsigned i = 0; i < PROC_TABLE; i++)
        if (!sessions[i].owner) {
            sessions[i].owner = p;
            sessions[i].pid = p->pid;
            return &sessions[i];
        }
    return NULL;
}

static struct node *lookup(struct process *p, uint64_t token, struct session **sp)
{
    if (!eligible(p) || token <= FILE_TOKEN_DOMAIN || token >> 63)
        return NULL;
    struct session *s = find(p);
    if (!s || s->closing)
        return NULL;
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (s->nodes[i].used == 2 && s->nodes[i].token == token) {
            *sp = s;
            return &s->nodes[i];
        }
    return NULL;
}

static struct node *reserve(struct session *s)
{
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!s->nodes[i].used) {
            s->nodes[i].used = 1;
            return &s->nodes[i];
        }
    return NULL;
}

static void release(struct session *s)
{
    if (s->active)
        panic("file: release of active session");
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (s->nodes[i].used == 2) {
            if (!volume.refs)
                panic("file: reference underflow");
            volume.refs--;
        }
    memset(s, 0, sizeof(*s));            /* also erase path/staging */
}

static int binding(void)
{
    struct blk_desc d;
    if (!volume.fs)
        return -FILE_EUNAVAIL;
    if (volume.dead)
        return -FILE_ESTALE;
    if (blk_describe(volume.device, &d) || d.epoch != volume.epoch ||
        d.reset_seq != volume.reset_seq || d.sectors != volume.sectors ||
        d.incarnation != volume.incarnation) {
        volume.dead = 1;                 /* irrevocable until explicit detach */
        return -FILE_ESTALE;
    }
    return d.state == BLK_DEV_READY ? 0 : -FILE_EUNAVAIL;
}

static int map_ext2(int err)
{
    switch (err) {
    case EXT2_OK: return 0;
    case EXT2_ENOENT: return -FILE_ENOENT;
    case EXT2_ENOTDIR: return -FILE_ENOTDIR;
    case EXT2_EISDIR: return -FILE_EISDIR;
    case EXT2_ELOOP: return -FILE_ELOOP;
    case EXT2_ENAMETOOLONG: return -FILE_ENAMETOOLONG;
    case EXT2_EBUSY: return -VOS_EAGAIN;
    case EXT2_ESTALE: return -FILE_ESTALE;
    case EXT2_EUNAVAIL: return -FILE_EUNAVAIL;
    case EXT2_EINTR: return -FILE_EINTR;
    case EXT2_EUNSUPP: return -FILE_EUNSUPP;
    case EXT2_EINVAL: case EXT2_ETOODEEP: return -VOS_EINVAL;
    default: return -FILE_EIO;           /* wrong filesystem, corruption, I/O */
    }
}

/* Bounded observer at I1 checkpoints, including immediately after waits.
 * No I/O, allocation, wait or retained session pointer.
 */
static int binding_guard(void)
{
    int r = binding();
    return r == -FILE_ESTALE ? IO_SCOPE_STALE : r ? IO_SCOPE_UNAVAILABLE : IO_SCOPE_OK;
}

static int outcome(struct session *s, int err)
{
    int stop = io_scope_check();
    if ((s && (s->closing || s->owner->kill_reason)) || stop == IO_SCOPE_INTERRUPTED)
        return -FILE_EINTR;
    if (stop == IO_SCOPE_TIMEOUT)
        return -FILE_ETIMEDOUT;
    int r = binding();
    if (r == -FILE_ESTALE)
        return r;
    if (stop == IO_SCOPE_BUDGET)
        return -VOS_EAGAIN;
    if (stop == IO_SCOPE_STALE)
        return -FILE_ESTALE;
    if (stop == IO_SCOPE_UNAVAILABLE)
        return -FILE_EUNAVAIL;
    return r ? r : map_ext2(err);
}

static int begin(struct session *s, struct io_scope *scope)
{
    if (s->active || s->closing)
        return -VOS_EAGAIN;
    if (s->owner->kill_reason)
        return -FILE_EINTR;
    if (io_scope_begin(scope, s->pid))
        return -VOS_EAGAIN;
    s->active = 1;
    s->tid = thread_current()->id;
    volume.active++;
    io_scope_set_guard(binding_guard);
    return 0;
}

static int finish(struct session *s, struct io_scope *scope)
{
    int r = outcome(s, EXT2_OK);
    io_scope_end(scope);
    s->active = 0;
    s->tid = 0;
    volume.active--;
    memset(s->stage, 0, sizeof(s->stage));
    memset(s->path, 0, sizeof(s->path));
    if (s->closing)
        release(s);                    /* only scalar debt may remain in I1 */
    return r;
}

/* Validate the entire byte path before lookup, even a doomed prefix. */
static int path_copy(struct session *s, const char *path, uint64_t len,
                     const struct exc_frame *frame)
{
    if (!len)
        return -VOS_EINVAL;
    if (len > EXT2_PATH_MAX)
        return -FILE_ENAMETOOLONG;
    if (frame) {
        if (copy_from_user(s->owner->root, s->path, frame->rsi, len))
            return -VOS_EFAULT;
    } else {
        if (!path)
            return -VOS_EINVAL;
        memcpy(s->path, path, len);
    }
    s->path[len] = 0;
    unsigned components = 0;
    for (uint64_t i = 0; i < len;) {
        uint64_t start = i;
        while (i < len && s->path[i] != '/') {
            if (!s->path[i])
                return -VOS_EINVAL;
            i++;
        }
        uint64_t n = i - start;
        if (!n)
            return -VOS_EINVAL;
        if (n == 2 && s->path[start] == '.' && s->path[start + 1] == '.')
            return -VOS_EACCES;
        if (n > EXT2_NAME_MAX)
            return -FILE_ENAMETOOLONG;
        if (++components > EXT2_MAX_COMPONENTS)
            return -VOS_EINVAL;
        if (i < len && ++i == len)
            return -VOS_EINVAL;
    }
    return 0;
}

static int node_stat(struct session *s, uint32_t ino, struct ext2_stat *st)
{
    int r = outcome(s, EXT2_OK);
    if (r)
        return r;
    memset(st, 0, sizeof(*st));
    return outcome(s, ext2_stat(volume.fs, ino, st));
}

static int walk(struct session *s, uint32_t base, uint64_t len, struct ext2_stat *st)
{
    int r = node_stat(s, base, st);
    if (r)
        return r;
    for (uint64_t i = 0; i < len;) {
        if (st->type != EXT2_T_DIR)
            return -FILE_ENOTDIR;
        uint64_t start = i;
        while (i < len && s->path[i] != '/')
            i++;
        uint64_t n = i - start;
        if (!(n == 1 && s->path[start] == '.')) {
            uint32_t ino;
            if ((r = outcome(s, EXT2_OK)))
                return r;
            r = ext2_lookup(volume.fs, st->ino, s->path + start, n, &ino);
            if ((r = outcome(s, r)))
                return r;
            if (ino == EXT2_ROOT_INO)
                return -VOS_EACCES;
            if ((r = node_stat(s, ino, st)))
                return r;
            if (st->type == EXT2_T_LNK)
                return -FILE_ELOOP;
            if (st->type != EXT2_T_REG && st->type != EXT2_T_DIR)
                return -FILE_EUNSUPP;
        }
        if (i < len)
            i++;
    }
    return st->ino == EXT2_ROOT_INO ? -VOS_EACCES : 0;
}

static void publish(struct node *n, const struct ext2_stat *st, uint64_t rights, uint64_t token)
{
    if (rights & ~FILE_RIGHTS)
        panic("file: publishing unvalidated rights");
    *n = (struct node){ .token = token, .generation = volume.generation, .ino = st->ino,
                       .rights = (unsigned)rights, .kind = st->type, .used = 2 };
    volume.refs++;
}

int file_volume_attach(struct process *owner, blk_id_t device)
{
    struct blk_desc before, after;
    struct io_scope scope;
    struct ext2_fs *fs = NULL;
    if (!eligible(owner) || owner->state != PROC_READY || owner->kill_reason)
        return -VOS_EINVAL;
    if (volume.fs || volume.config_owner || io_scope_count())
        return -VOS_EAGAIN;
    if (next_volume == UINT64_MAX)
        return -VOS_ENOMEM;
    if (blk_describe(device, &before) || before.state != BLK_DEV_READY || !(before.caps & BLK_CAP_READ))
        return -FILE_EUNAVAIL;
    if (io_scope_begin(&scope, owner->pid))
        return -VOS_EAGAIN;
    volume.config_owner = owner;         /* pins owner until setup has unwound */
    volume.config_pid = owner->pid;
    volume.config_tid = thread_current()->id;
    int err = ext2_open(device, &fs), r = map_ext2(err);
    int stop = io_scope_check();
    if (owner->kill_reason || stop == IO_SCOPE_INTERRUPTED)
        r = -FILE_EINTR;
    else if (stop == IO_SCOPE_TIMEOUT)
        r = -FILE_ETIMEDOUT;
    else if (blk_describe(device, &after) || after.epoch != before.epoch ||
             after.reset_seq != before.reset_seq || after.sectors != before.sectors ||
             after.incarnation != before.incarnation)
        r = -FILE_ESTALE;
    else if (stop == IO_SCOPE_BUDGET)
        r = -VOS_EAGAIN;
    else if (after.state != BLK_DEV_READY)
        r = -FILE_EUNAVAIL;
    io_scope_end(&scope);
    volume.config_owner = NULL;
    volume.config_pid = volume.config_tid = 0;
    if (r) {
        if (fs && ext2_close(fs))
            panic("file: failed attach reader not releasable");
        return r;
    }
    volume.fs = fs;
    volume.device = device;
    volume.epoch = before.epoch;
    volume.reset_seq = before.reset_seq;
    volume.sectors = before.sectors;
    volume.incarnation = before.incarnation;
    volume.generation = next_volume++;
    return 0;
}

int file_volume_detach(void)
{
    if (volume.config_owner || volume.refs || volume.active || io_scope_count())
        return -VOS_EAGAIN;
    if (volume.fs && ext2_close(volume.fs))
        return -VOS_EAGAIN;
    memset(&volume, 0, sizeof(volume));
    return 0;
}

int file_grants_install(struct process *p, const struct file_grant *grants, unsigned count)
{
    if (!eligible(p) || p->state != PROC_READY || p->tid || count > FILE_INITIAL_MAX ||
        (count && !grants) || p->kill_reason)
        return -VOS_EINVAL;
    if (find(p))
        return -VOS_EAGAIN;              /* one atomic provisioning, never recreate grants */
    unsigned seen = 0;
    for (unsigned i = 0; i < count; i++) {
        if (grants[i].index >= FILE_INITIAL_MAX || (grants[i].rights & ~FILE_RIGHTS) ||
            (seen & (1u << grants[i].index))) {
            return -VOS_EINVAL;
        }
        seen |= 1u << grants[i].index;
    }
    /* Validate all descriptor scalars before reserving any session or doing
     * path work. Unknown high bits cannot disappear at a narrow parameter.
     */
    struct session *s = allocate(p);
    if (!s)
        return -VOS_ENOMEM;
    int r = 0;
    for (unsigned i = 0; i < count; i++) {
        if ((r = path_copy(s, grants[i].path, grants[i].length, NULL)))
            goto fail;
    }
    if (!count) {
        s->provisioned = 1;              /* grant-free launch needs no disk */
        return 0;
    }
    struct io_scope scope;
    if ((r = begin(s, &scope)))
        goto fail;
    struct ext2_stat st[FILE_INITIAL_MAX];
    uint64_t tokens[FILE_INITIAL_MAX] = {0};
    for (unsigned i = 0; i < count; i++) {
        path_copy(s, grants[i].path, grants[i].length, NULL); /* immutable trusted setup descriptors */
        r = walk(s, EXT2_ROOT_INO, grants[i].length, &st[i]);
        if (!r && st[i].type != EXT2_T_DIR)
            r = -FILE_ENOTDIR;
        if (r)
            break;
        tokens[i] = issue(&next_token);
        if (!tokens[i]) {
            r = -VOS_ENOMEM;
            break;
        }
    }
    int final = finish(s, &scope);
    if (final)
        r = final;
    if (r)
        goto fail;
    for (unsigned i = 0; i < count; i++) {
        publish(&s->nodes[i], &st[i], grants[i].rights, tokens[i]);
        s->initial[grants[i].index] = tokens[i];
    }
    s->provisioned = 1;
    return 0;
fail:
    if (s->owner)                      /* finish may already retire a cancelled session */
        release(s);                    /* no publication on failed setup */
    return r;
}

int64_t file_initial(struct process *p, uint64_t index)
{
    if (!eligible(p))
        return -VOS_EBADHANDLE;
    if (index >= FILE_INITIAL_MAX)
        return -VOS_EINVAL;
    struct session *s = find(p);
    return s && !s->closing && s->initial[index] ? (int64_t)s->initial[index] : -VOS_EBADHANDLE;
}

static int64_t open_at(struct process *p, uint64_t base, const char *path, uint64_t len,
                       uint64_t rights, const struct exc_frame *frame)
{
    struct session *s;
    struct node *n = lookup(p, base, &s);
    if (!n)
        return -VOS_EBADHANDLE;
    if (n->kind != EXT2_T_DIR)
        return -FILE_ENOTDIR;
    if (!(n->rights & FILE_TRAVERSE))
        return -VOS_EACCES;
    if (!len)
        return -VOS_EINVAL;
    if (len > EXT2_PATH_MAX)
        return -FILE_ENAMETOOLONG;
    if (rights & ~FILE_RIGHTS)
        return -VOS_EINVAL;
    if (rights & ~(uint64_t)n->rights)
        return -VOS_EACCES;
    if (s->active)
        return -VOS_EAGAIN;
    int r = path_copy(s, path, len, frame);
    if (r)
        return r;
    struct node *slot = reserve(s);
    if (!slot)
        return -VOS_ENOMEM;
    struct io_scope scope;
    if ((r = begin(s, &scope))) {
        memset(slot, 0, sizeof(*slot));
        return r;
    }
    struct ext2_stat st;
    r = n->generation != volume.generation ? -FILE_ESTALE : walk(s, n->ino, len, &st);
    int final = finish(s, &scope);
    if (final)
        r = final;
    uint64_t token = r ? 0 : issue(&next_token);
    if (!r && !token)
        r = -VOS_ENOMEM;
    if (r) {
        memset(slot, 0, sizeof(*slot));
        return r;
    }
    publish(slot, &st, rights, token);
    return (int64_t)token;
}

int64_t file_open_at(struct process *p, uint64_t base, const char *path, uint64_t len, uint64_t rights)
{
    return open_at(p, base, path, len, rights, NULL);
}

int64_t file_close(struct process *p, uint64_t token)
{
    struct session *s;
    struct node *n = lookup(p, token, &s);
    if (!n)
        return -VOS_EBADHANDLE;
    if (s->active)
        return -VOS_EAGAIN;
    for (unsigned i = 0; i < FILE_INITIAL_MAX; i++)
        if (s->initial[i] == token)
            s->initial[i] = 0;
    memset(n, 0, sizeof(*n));
    volume.refs--;
    return 0;
}

static int access_node(struct process *p, uint64_t token, unsigned kind, unsigned right,
                       struct session **s, struct node **node)
{
    struct node *n = lookup(p, token, s);
    if (!n)
        return -VOS_EBADHANDLE;
    if (kind && n->kind != kind)
        return kind == EXT2_T_REG ? -FILE_EISDIR : -FILE_ENOTDIR;
    if (!(n->rights & right))
        return -VOS_EACCES;
    *node = n;
    return 0;
}

static int stat_node(struct process *p, uint64_t token, struct ext2_stat *out,
                     const struct exc_frame *frame)
{
    struct session *s;
    struct node *n;
    int r = access_node(p, token, 0, FILE_STAT, &s, &n);
    if (r)
        return r;
    if (frame && frame->rdx != 64)
        return -VOS_EINVAL;
    if (frame && user_writable(p->root, frame->rsi, 64))
        return -VOS_EFAULT;
    if (!frame && !out)
        return -VOS_EINVAL;
    struct io_scope scope;
    if ((r = begin(s, &scope)))
        return r;
    struct ext2_stat st;
    r = node_stat(s, n->ino, &st);
    int final = finish(s, &scope);
    if (final)
        r = final;
    if (!r) {
        if (frame) {
            /* Fixed little-endian wire fields, no native padding or inode. */
            memset(s->stage, 0, 64);
            uint64_t sizes[2] = {st.size, st.blocks512};
            uint32_t fields[6] = {st.type, st.mode, st.uid, st.gid, st.links, n->rights};
            memcpy(s->stage, sizes, sizeof(sizes));
            memcpy(s->stage + 16, fields, sizeof(fields));
            r = copy_to_user(p->root, frame->rsi, s->stage, 64);
            memset(s->stage, 0, 64);
        } else
            memcpy(out, &st, sizeof(st));
    }
    return r;
}

int file_stat(struct process *p, uint64_t token, struct ext2_stat *out)
{
    return stat_node(p, token, out, NULL);
}

static int64_t read_at(struct process *p, uint64_t token, void *out, uint64_t len,
                       uint64_t off, const struct exc_frame *frame)
{
    struct session *s;
    struct node *n;
    int r = access_node(p, token, EXT2_T_REG, FILE_READ, &s, &n);
    if (r)
        return r;
    if (len > 4096 || off > UINT64_MAX - len)
        return -VOS_EINVAL;
    if (!len)
        return 0;
    if (frame && user_writable(p->root, frame->rsi, len))
        return -VOS_EFAULT;
    if (!frame && !out)
        return -VOS_EINVAL;
    struct io_scope scope;
    if ((r = begin(s, &scope)))
        return r;
    uint64_t done = 0;
    r = outcome(s, EXT2_OK);
    if (!r)
        r = outcome(s, ext2_read(volume.fs, n->ino, off, s->stage, len, &done));
    /* Commit before the no-wait finish erases staging. IF=0: no stop can arise
     * between this last outcome check, copy and finish. Never copy on error.
     */
    if (!r && !(r = outcome(s, EXT2_OK))) {
        if (frame) {
            /* Decode again from the live architectural frame after all waits.
             * Validate requested length even for EOF/short output. No user
             * address is stored in a session, scope, driver or continuation.
             */
            r = user_writable(p->root, frame->rsi, len);
            if (!r)
                r = copy_to_user(p->root, frame->rsi, s->stage, done);
        } else
            memcpy(out, s->stage, done);
    }
    int final = finish(s, &scope);
    return r ? r : final ? final : (int64_t)done;
}

int64_t file_read_at(struct process *p, uint64_t token, void *out, uint64_t len, uint64_t off)
{
    return read_at(p, token, out, len, off, NULL);
}

static int next_entry(struct process *p, uint64_t token, uint64_t *cookie,
                      struct ext2_dirent *out, const struct exc_frame *frame)
{
    struct session *s;
    struct node *n;
    int r = access_node(p, token, EXT2_T_DIR, FILE_ENUMERATE, &s, &n);
    if (r)
        return r;
    if (frame && frame->r10 != 272)
        return -VOS_EINVAL;
    if (frame && user_writable(p->root, frame->rdx, 272))
        return -VOS_EFAULT;
    if (!frame && (!cookie || !out))
        return -VOS_EINVAL;
    struct io_scope scope;
    if ((r = begin(s, &scope)))
        return r;
    struct ext2_stat st;
    struct ext2_dirent ent;
    uint64_t pos = frame ? frame->rsi : *cookie;
    int eof = 1;
    r = node_stat(s, n->ino, &st);
    uint64_t size = r ? 0 : st.size;
    if (!r && pos > size)
        r = -VOS_EINVAL;
    while (!r && pos < size) {
        memset(&ent, 0, sizeof(ent));
        int err = ext2_readdir(volume.fs, n->ino, &pos, &ent);
        r = outcome(s, err == EXT2_EOD ? EXT2_OK : err);
        if (r || err == EXT2_EOD)
            break;
        if ((ent.name_len == 1 && ent.name[0] == '.') ||
            (ent.name_len == 2 && ent.name[0] == '.' && ent.name[1] == '.'))
            continue;
        r = node_stat(s, ent.ino, &st);
        if (!r && ent.type != EXT2_T_UNKNOWN && ent.type != st.type)
            r = -FILE_EIO;
        if (!r)
            ent.type = st.type;
        eof = 0;
        break;
    }
    int final = finish(s, &scope);
    if (final)
        r = final;
    if (!r && !eof) {
        if (frame) {
            memset(s->stage, 0, 272);
            uint32_t fields[2] = {ent.type, ent.name_len};
            memcpy(s->stage, &pos, 8);
            memcpy(s->stage + 8, fields, 8);
            memcpy(s->stage + 16, ent.name, ent.name_len);
            r = copy_to_user(p->root, frame->rdx, s->stage, 272);
            memset(s->stage, 0, 272);
        } else {
            memcpy(out, &ent, sizeof(ent));
            *cookie = pos;
        }
    }
    return r ? r : eof ? 0 : 1;
}

int file_next(struct process *p, uint64_t token, uint64_t *cookie, struct ext2_dirent *out)
{
    return next_entry(p, token, cookie, out, NULL);
}

/* Only syscall.c's ABI gate calls this bridge. All pointer registers stay in
 * the live trap frame. Common implementations receive NULL kernel pointers.
 * No second filesystem layer, allocation or additional large buffer.
 */
int64_t file_syscall(struct process *p, const struct exc_frame *frame)
{
    switch (frame->rax) {
    case SYS_FILE_OPEN_AT:
        return open_at(p, frame->rdi, NULL, frame->rdx, frame->r10, frame);
    case SYS_FILE_READ_AT:
        return read_at(p, frame->rdi, NULL, frame->rdx, frame->r10, frame);
    case SYS_FILE_STAT:
        return stat_node(p, frame->rdi, NULL, frame);
    case SYS_DIRECTORY_NEXT:
        return next_entry(p, frame->rdi, NULL, NULL, frame);
    case SYS_FILE_INITIAL:
        return file_initial(p, frame->rdi);
    default:
        return -VOS_ENOSYS;
    }
}

void file_close_all(struct process *p)
{
    if (volume.config_owner == p && p) {
        io_scope_cancel(volume.config_pid);
        thread_interrupt(volume.config_tid);
    }
    struct session *s = find(p);
    if (!s)
        return;
    if (s->active) {
        s->closing = 1;
        io_scope_cancel(s->pid);
        thread_interrupt(s->tid);
    } else {
        release(s);
    }
}

int file_process_busy(const struct process *p)
{
    struct session *s = find(p);
    return (s && s->active) || (p && volume.config_owner == p);
}

unsigned file_handle_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < PROC_TABLE; i++)
        for (unsigned j = 0; j < FILE_MAX; j++)
            n += sessions[i].nodes[j].used == 2;
    return n;
}

unsigned file_session_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < PROC_TABLE; i++)
        n += sessions[i].owner != NULL;
    return n;
}

const char *file_audit(void)
{
    unsigned refs = 0, active = 0;
    for (unsigned i = 0; i < PROC_TABLE; i++) {
        const struct session *s = &sessions[i];
        if (!s->owner) {
            const uint8_t *b = (const uint8_t *)s;
            for (unsigned k = 0; k < sizeof(*s); k++)
                if (b[k])
                    return "free file session retains data";
            continue;
        }
        if (!proc_file_live(s->owner) || s->owner->pid != s->pid || s->active > 1 ||
            (!s->active && !s->provisioned))
            return "file session has invalid process owner";
        for (unsigned j = 0; j < i; j++)
            if (sessions[j].owner == s->owner || (sessions[j].owner && sessions[j].pid == s->pid))
                return "duplicate file session";
        if (s->active != (s->tid != 0) ||
            (s->active && (thread_state(s->tid) == THREAD_FREE || thread_state(s->tid) == THREAD_DEAD)) ||
            (!s->active && s->closing))
            return "file operation has dangling owner";
        active += s->active;
        for (unsigned j = 0; j < FILE_MAX; j++) {
            const struct node *n = &s->nodes[j];
            if (n->used == 2) {
                if (!volume.fs || n->generation != volume.generation || n->ino == EXT2_ROOT_INO ||
                    !n->ino || n->token <= FILE_TOKEN_DOMAIN || n->token >> 63 ||
                    (n->rights & ~FILE_RIGHTS) || (n->kind != EXT2_T_REG && n->kind != EXT2_T_DIR))
                    return "invalid published file node";
                for (unsigned a = 0; a <= i; a++)
                    for (unsigned b = 0; b < FILE_MAX; b++)
                        if ((a != i || b != j) && sessions[a].nodes[b].used == 2 &&
                            sessions[a].nodes[b].token == n->token)
                            return "duplicate file token";
                refs++;
            } else if (n->used > 1 || (n->used && !s->active)) {
                return "file reservation outlived operation";
            } else if (n->token || n->generation || n->ino || n->rights || n->kind) {
                return "unpublished file node retains data";
            }
        }
        for (unsigned j = 0; j < FILE_INITIAL_MAX; j++) {
            int found = !s->initial[j];
            for (unsigned k = 0; k < FILE_MAX; k++)
                found |= s->nodes[k].used == 2 && s->nodes[k].token == s->initial[j];
            if (!found)
                return "initial grant does not name a live node";
        }
    }
    if (refs != volume.refs || active != volume.active || active > IO_SCOPE_TICKETS)
        return "file volume reference mismatch";
    if (active + (volume.config_owner != NULL) > io_scope_count())
        return "file operation has no admission ticket";
    if (volume.config_owner && (!proc_file_live(volume.config_owner) ||
        volume.config_owner->pid != volume.config_pid || !volume.config_tid ||
        thread_state(volume.config_tid) == THREAD_FREE || thread_state(volume.config_tid) == THREAD_DEAD))
        return "file volume setup has dangling owner";
    return io_scope_audit();
}

#ifdef VOS_TEST
uint64_t file_test_issue(uint64_t *serial) { return issue(serial); }
void file_test_last_tokens(void)
{
    if (next_token >= FILE_SERIAL_MAX - 1)
        panic("file: token test already exhausted");
    next_token = FILE_SERIAL_MAX - 1;
}
#endif

#ifdef VOS_TEST
#include "storage_observe.h"

void file_test_observe(struct observe_file *out)
{
    out->fs = (uint64_t)volume.fs;
    out->generation = volume.generation;
    out->incarnation = volume.incarnation;
    out->epoch = volume.epoch;
    out->reset_seq = volume.reset_seq;
    out->sectors = volume.sectors;
    out->config_owner = (uint64_t)volume.config_owner;
    out->config_pid = volume.config_pid;
    out->config_tid = volume.config_tid;
    out->device = volume.device;
    out->refs = volume.refs;
    out->active = volume.active;
    out->dead = volume.dead;
    for (unsigned i = 0; i < PROC_TABLE; i++) {
        const struct session *s = &sessions[i];
        struct observe_session *d = &out->sessions[i];
        d->owner = (uint64_t)s->owner;
        d->pid = s->pid;
        d->tid = s->tid;
        memcpy(d->initial, s->initial, sizeof(d->initial));
        d->active = s->active;
        d->closing = s->closing;
        d->provisioned = s->provisioned;
        for (unsigned j = 0; j < FILE_MAX; j++) {
            const struct node *n = &s->nodes[j];
            d->nodes[j] = (struct observe_node){n->token, n->generation, n->ino,
                n->rights, n->kind, n->used};
        }
    }
}
#endif

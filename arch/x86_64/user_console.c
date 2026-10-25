#include "user_console.h"
#include "process.h"
#include "exception.h"
#include "syscall.h"
#include "usercopy.h"
#include "keyboard.h"
#include "input_console.h"
#include "console.h"
#include "screen.h"
#include "thread.h"
#include "cpu.h"
#include "exec.h"
#include "vmm.h"
#include "panic.h"

static struct process *owner;
static uint64_t owner_pid;
static struct keyboard keyboard;
static char pending[INPUT_CONSOLE_LINE_MAX + 1];
static int pending_length = -1;
#ifdef VOS_CONSOLE_HOST
static uint8_t port_in(uint16_t port){return console_host_in(port);}
static void port_out(uint16_t port,uint8_t data){console_host_out(port,data);}
#else
static uint8_t port_in(uint16_t port){return inb(port);}
static void port_out(uint16_t port,uint8_t data){outb(port,data);}
#endif
static const struct keyboard_io ports = { port_in, port_out };

static uint64_t epoch;
static struct process *reader;
static uint64_t reader_pid;
static struct console_lease_stats stats;
static struct {
    int active,revoked;
    struct process *parent,*child;
    uint64_t parent_pid,parent_tid,child_pid,expected_epoch;
} lease;
static void advance(void)
{
    if(epoch==UINT64_MAX)panic("console authority generation exhausted");
    epoch++;
}
static void clear_pending(void)
{
    pending_length=-1;
    for(unsigned i=0;i<sizeof(pending);i++)pending[i]=0;
}
static void broker_context(void)
{
#ifdef VOS_CONSOLE_HOST
    if(console_host_root()!=vmm_kernel_root())panic("console lease outside kernel CR3");
#else
    if((read_cr3()&PTE_ADDR)!=vmm_kernel_root() || (read_rflags()&(1ULL<<9)))
        panic("console lease outside kernel context");
#endif
}
int user_console_owned(const struct process *p)
{
    return p && p==owner && proc_file_live(p) && p->pid==owner_pid && !p->kill_reason;
}
int user_console_reader_active(const struct process *p){return reader==p && p && reader_pid==p->pid;}
int user_console_debug_visible(const struct process *p)
{
    return owner ? user_console_owned(p) : !lease.active;
}
int user_console_lease_active(void){return lease.active;}
int user_console_exec_active(const struct process *p,const struct process *child)
{
    return lease.active && lease.parent==p && lease.child==child &&
        p->pid==lease.parent_pid && child->pid==lease.child_pid;
}
const struct console_lease_stats *user_console_lease_stats(void){return &stats;}

int user_console_grant(struct process *p)
{
    if(owner || lease.active || !proc_file_live(p) || p->state!=PROC_READY ||
       (p->abi!=3 && p->abi!=4) || p->kill_reason)return -VOS_EACCES;
    if(!keyboard_init(&keyboard,&ports))return -VOS_EUNAVAIL;
    advance();owner=p;owner_pid=p->pid;clear_pending();
    screen_begin_input();screen_status(SCREEN_PASS,"native console ready");
    return 0;
}
static void drop_owner(void)
{
    if(owner) {
        (void)keyboard_suspend(&keyboard);
        owner=0;owner_pid=0;clear_pending();advance();
        if(lease.active)lease.expected_epoch=epoch;
    }
}
void user_console_revoke(struct process *p)
{
    if(!p)return;
    if(lease.active && p==lease.parent && p->pid==lease.parent_pid) {
        lease.revoked=1;stats.revoked++;
        drop_owner();exec_cancel_request(p);
        return;
    }
    if(p==owner && p->pid==owner_pid)drop_owner();
}
int user_console_begin_exec(struct process *p,struct process *child)
{
    broker_context();
    if(lease.active || reader || !user_console_owned(p) || p->state!=PROC_RUNNING ||
       !proc_file_live(child) || child->state!=PROC_READY || child->tid || child->kill_reason)
        return -VOS_EACCES;
    if(!keyboard_suspend(&keyboard)){user_console_revoke(p);return -VOS_EUNAVAIL;}
    lease.active=1;lease.revoked=0;lease.parent=p;lease.parent_pid=p->pid;
    lease.parent_tid=p->tid;lease.child=child;lease.child_pid=child->pid;
    clear_pending();advance();owner=child;owner_pid=child->pid;lease.expected_epoch=epoch;
    if(!keyboard_resume(&keyboard)){owner=0;owner_pid=0;return -VOS_EUNAVAIL;}
    stats.transferred++;
    serial_puts("console: foreground child ");serial_dec(child->pid);serial_puts("\r\n");
    return 0;
}
int user_console_finish_exec(struct process *p,struct process *child,int restore_allowed)
{
    broker_context();
    if(!lease.active || p!=lease.parent || child!=lease.child)return -VOS_EINVAL;
    if(reader)panic("console reader survived child reclamation");
    int valid=restore_allowed && !lease.revoked && !owner && lease.expected_epoch==epoch &&
        proc_file_live(p) && p->state==PROC_RUNNING && p->pid==lease.parent_pid &&
        p->tid==lease.parent_tid && !p->kill_reason;
    if(owner==child && owner_pid==lease.child_pid){drop_owner();valid=0;}
    clear_pending();lease=(__typeof__(lease)){0};
    if(!valid){stats.abandoned++;return 0;}
    advance();owner=p;owner_pid=p->pid;
    if(!keyboard_resume(&keyboard)){owner=0;owner_pid=0;stats.abandoned++;return -VOS_EUNAVAIL;}
    stats.restored++;
    serial_puts("console: restored parent ");serial_dec(p->pid);serial_puts("\r\n");
    return 0;
}

static int cancelled(void *ctx)
{
    struct process *p = ctx;
    return owner != p || owner_pid != p->pid || p->kill_reason != 0 ||
           (lease.active && lease.revoked);
}

int64_t user_console_syscall(struct process *p, const struct exc_frame *frame)
{
    if (p != owner || !proc_file_live(p) || owner_pid != p->pid || p->state != PROC_RUNNING ||
        thread_current()->proc != p || thread_current()->id != p->tid)
        return -VOS_EACCES;
    if (cancelled(p))
        return -VOS_EINTR;
    uint64_t pointer = frame->rdi, size = frame->rsi;
    if (frame->rax == SYS_CONSOLE_CLEAR) {
        screen_begin_input();
        return 0;
    }
    if (frame->rax == SYS_CONSOLE_WRITE) {
        char text[256] = { 0 };
        if (size > sizeof(text)) return -VOS_EINVAL;
        if (copy_from_user(p->root, text, pointer, size)) return -VOS_EFAULT;
        /* Untrusted application output stays prefixed in diagnostics. Only
         * its explicitly granted display stream is unprefixed. No ANSI parser.
         */
        debug_emit(p->name, text, size, serial_log_putc);
        for (unsigned i = 0; i < size; i++)
            screen_putc(text[i] == '\n' || (text[i] >= ' ' && text[i] <= '~') ? text[i] : '?');
        return (int64_t)size;
    }
    if (frame->rax != SYS_CONSOLE_READ) return -VOS_ENOSYS;
    if (!size || size > sizeof(pending)) return -VOS_EINVAL;
    if (user_writable(p->root, pointer, size)) return -VOS_EFAULT;
    if (pending_length < 0) {
        if(reader)return -VOS_EAGAIN;
        reader=p;reader_pid=p->pid;
        int result = input_console_read_line_for(&keyboard, pending, sizeof(pending), "", cancelled, p);
        reader=0;reader_pid=0;
        if (result < 0) return result == INPUT_CONSOLE_INTERRUPTED ? -VOS_EINTR : -VOS_EUNAVAIL;
        pending_length = result;
    }
    if (cancelled(p)) return -VOS_EINTR;
    /* Waits can run other threads. Decode the still-live trap frame again and
     * validate all output pages before committing any byte. Retain on errors.
     */
    pointer = frame->rdi; size = frame->rsi;
    if (!size || size > sizeof(pending)) return -VOS_EINVAL;
    if (user_writable(p->root, pointer, size)) return -VOS_EFAULT;
    if (size <= (unsigned)pending_length) return -VOS_EMSGSIZE;
    if (copy_to_user(p->root, pointer, pending, pending_length + 1)) return -VOS_EFAULT;
    int result = pending_length;
    pending_length = -1;
    return result;
}

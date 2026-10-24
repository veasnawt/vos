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

static struct process *owner;
static uint64_t owner_pid;
static struct keyboard keyboard;
static char pending[INPUT_CONSOLE_LINE_MAX + 1];
static int pending_length = -1;
static uint8_t port_in(uint16_t port) { return inb(port); }
static void port_out(uint16_t port, uint8_t data) { outb(port, data); }
static const struct keyboard_io ports = { port_in, port_out };

int user_console_grant(struct process *p)
{
    if (owner || !proc_file_live(p) || p->state != PROC_READY || p->abi != 3)
        return -VOS_EACCES;
    if (!keyboard_init(&keyboard, &ports))
        return -VOS_EUNAVAIL;
    owner = p;
    owner_pid = p->pid;
    pending_length = -1;
    screen_begin_input();
    screen_status(SCREEN_PASS, "native console ready");
    return 0;
}

void user_console_revoke(struct process *p)
{
    if (p != owner)
        return;
    owner = 0;
    owner_pid = 0;
    pending_length = -1;
    for (unsigned i = 0; i < sizeof(pending); i++) pending[i] = 0;
}

static int cancelled(void *ctx)
{
    struct process *p = ctx;
    return owner != p || owner_pid != p->pid || p->kill_reason != 0;
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
        int result = input_console_read_line_for(&keyboard, pending, sizeof(pending), "", cancelled, p);
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

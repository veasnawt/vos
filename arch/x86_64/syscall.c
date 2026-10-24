#include "syscall.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "process.h"
#include "thread.h"
#include "usercopy.h"
#include "file.h"
#include "user_console.h"

void debug_emit(const char *name, const char *data, uint64_t len, void (*put)(char))
{
    int line_start = 1;

    for (uint64_t i = 0; i < len; i++) {
        char c = data[i];

        if (line_start) {
            for (const char *s = "user["; *s; s++)
                put(*s);
            for (const char *s = name; *s; s++)
                put(*s);
            put(']');
            put(':');
            put(' ');
            line_start = 0;
        }
        if (c == '\n') {
            put('\r');
            put('\n');
            line_start = 1;
        } else {
            put(c >= 0x20 && c < 0x7F ? c : '?');
        }
    }
    if (!line_start) {
        put('\r');
        put('\n');
    }
}

/* debug_write(buf, len): copy into a kernel buffer first, then print only that copy. */
static int64_t sys_debug_write(struct process *p, uint64_t buf, uint64_t len)
{
    char kbuf[DEBUG_WRITE_MAX];

    if (len > DEBUG_WRITE_MAX)
        return -VOS_EINVAL;
    if (copy_from_user(p->root, kbuf, buf, len) != 0)
        return -VOS_EFAULT;
    debug_emit(p->name, kbuf, len, serial_putc);
    return (int64_t)len;
}

int64_t syscall_dispatch(struct process *p, struct exc_frame *f)
{
    switch (f->rax) {
    case SYS_CONSOLE_READ:
    case SYS_CONSOLE_WRITE:
    case SYS_CONSOLE_CLEAR:
        if (p->abi < 3)
            return -VOS_ENOSYS;
        return user_console_syscall(p, f);
    case SYS_DEBUG_WRITE:
        return sys_debug_write(p, f->rdi, f->rsi);
    /* ABI >=1; v2 extends only close's accepted token domain. */
    case SYS_YIELD:
    case SYS_CHANNEL_SEND:
    case SYS_CHANNEL_RECV:
    case SYS_HANDLE_CLOSE:
        if (p->abi < 1)
            return -VOS_ENOSYS;
        switch (f->rax) {
        case SYS_YIELD:
            thread_yield();
            return 0;
        case SYS_CHANNEL_SEND:
            return channel_send(p, (int64_t)f->rdi, f->rsi, f->rdx);
        case SYS_CHANNEL_RECV:
            return channel_recv(p, (int64_t)f->rdi, f->rsi, f->rdx, 1);
        default:
            if (p->abi >= 2 && f->rdi > FILE_TOKEN_DOMAIN && !(f->rdi >> 63))
                return file_close(p, f->rdi);
            return handle_close(p, (int64_t)f->rdi);
        }
    case SYS_FILE_OPEN_AT:
    case SYS_FILE_READ_AT:
    case SYS_FILE_STAT:
    case SYS_DIRECTORY_NEXT:
    case SYS_FILE_INITIAL:
        if (p->abi < 2)
            return -VOS_ENOSYS;
        return file_syscall(p, f);
#ifdef VOS_TEST
    case SYS_TEST_KERNEL_AC: {
        uint64_t flags;
        __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags));
        return (flags >> 18) & 1;
    }
    case SYS_TEST_KERNEL_IF:
        return (read_rflags() & RFLAGS_IF) != 0;
    case SYS_TEST_TOUCH_USER:
        return *(volatile uint8_t *)f->rdi;     /* SMAP must stop this */
#endif
    default:
        return -VOS_ENOSYS;
    }
}

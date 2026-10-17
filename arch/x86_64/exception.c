#include "exception.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "panic.h"
#include "process.h"
#include "idt.h"

static const struct { const char *name, *msg; } exc_info[32] = {
    [0]  = { "#DE", "#DE divide error" },
    [1]  = { "#DB", "#DB debug" },
    [2]  = { "NMI", "NMI non-maskable interrupt" },
    [3]  = { "#BP", "#BP breakpoint" },
    [4]  = { "#OF", "#OF overflow" },
    [5]  = { "#BR", "#BR bound range exceeded" },
    [6]  = { "#UD", "#UD invalid opcode" },
    [7]  = { "#NM", "#NM device not available" },
    [8]  = { "#DF", "#DF double fault" },
    [9]  = { "#CSO", "#CSO coprocessor segment overrun" },
    [10] = { "#TS", "#TS invalid TSS" },
    [11] = { "#NP", "#NP segment not present" },
    [12] = { "#SS", "#SS stack-segment fault" },
    [13] = { "#GP", "#GP general protection fault" },
    [14] = { "#PF", "#PF page fault" },
    [15] = { "#15", "reserved vector 15" },
    [16] = { "#MF", "#MF x87 floating-point error" },
    [17] = { "#AC", "#AC alignment check" },
    [18] = { "#MC", "#MC machine check" },
    [19] = { "#XM", "#XM SIMD floating-point exception" },
    [20] = { "#VE", "#VE virtualization exception" },
    [21] = { "#CP", "#CP control protection exception" },
    [22] = { "#22", "reserved vector 22" },
    [23] = { "#23", "reserved vector 23" },
    [24] = { "#24", "reserved vector 24" },
    [25] = { "#25", "reserved vector 25" },
    [26] = { "#26", "reserved vector 26" },
    [27] = { "#27", "reserved vector 27" },
    [28] = { "#HV", "#HV hypervisor injection exception" },
    [29] = { "#VC", "#VC VMM communication exception" },
    [30] = { "#SX", "#SX security exception" },
    [31] = { "#31", "reserved vector 31" },
};

const char *exception_name(uint64_t vector)
{
    return vector < 32 ? exc_info[vector].msg : vector == IDT_SYSCALL ? "system call" : "?";
}

static int has_error_code(uint64_t v)
{
    return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30;
}

/*
 * Classification by the CPU-pushed CS: RPL 3 means the trap interrupted ring 3
 * and belongs to the current process (proc_user_trap does not return). Any
 * trap from ring 0 is a kernel error: report, then panic.
 */
uint64_t exception_handler(struct exc_frame *f)
{
    if ((f->cs & 3) != 0)
        return proc_user_trap(f);
    if (f->cs != GDT_KERNEL_CS)
        panic("trap with an unexpected code segment");
    if (f->vector == IDT_SYSCALL) {
        serial_puts("exc: int $0x80 from ring 0, rip=");
        serial_hex(f->rip);
        serial_puts("\r\n");
        panic("int $0x80 from kernel mode");
    }

    uint64_t v = f->vector < 32 ? f->vector : 31;

    serial_puts("exc: ");
    serial_puts(exc_info[v].name);
    serial_puts(" vec=");
    serial_dec(f->vector);
    serial_puts(" err=");
    if (has_error_code(v))
        serial_hex(f->error);
    else
        serial_puts("-");
    serial_puts(" cs=");
    serial_hex(f->cs);
    serial_puts(" rip=");
    serial_hex(f->rip);
    serial_puts(" rflags=");
    serial_hex(f->rflags);
    if (v == 14) {
        serial_puts(" cr2=");
        serial_hex(read_cr2());
    }
    serial_puts(" ss=");
    serial_hex(f->ss);
    serial_puts(" rsp=");
    serial_hex(f->rsp);
    serial_puts(gdt_in_ist1_stack((uint64_t)f) ? " stack=ist1\r\n" : " stack=kernel\r\n");

    panic(exc_info[v].msg);
}

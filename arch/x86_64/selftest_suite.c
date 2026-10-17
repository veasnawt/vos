/*
 * N1.9 isolation suite (VOS_TEST_CASE=suite).
 *
 * Every GRUB module whose command line starts with "t-" is one case. The
 * rest of the command line is the expected outcome, written in the suite's
 * GRUB configuration (trusted test configuration), never taken from the
 * program:
 *   expect=exit|fault|reject|umip|smapac
 *   status=N                 exit status (expect=exit)
 *   vec=N err=N              exception vector and error code (expect=fault)
 *   cr2=ADDR|stack|guard|image   fault address, exact or a region
 *   ripcr2                   the faulting RIP equals CR2 (execution faults)
 *   ok= enosys= einval= efault=  kernel-counted system-call results
 *   elf=NAME                 loader error (expect=reject)
 *   nohello                  do not rerun hello after this case
 * umip: #GP(0) if CR4.UMIP is set, else exit 0. smapac: exit 0 if SMAP is
 * enabled (the kernel saw AC clear), else exit 1.
 *
 * Each case is loaded with load_elf and run with proc_run, exactly like the
 * boot application. Its verdict is printed by the kernel ("suite: <name>:
 * PASS|FAIL"); program output is only ever a user[...] line.
 */
#include "selftest.h"
#include "app.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "process.h"
#include "vmm.h"

#define EXP_EXIT    1
#define EXP_FAULT   2
#define EXP_REJECT  3
#define EXP_UMIP    4
#define EXP_SMAPAC  5

#define CR2_ANY     0
#define CR2_EXACT   1
#define CR2_STACK   2
#define CR2_GUARD   3
#define CR2_IMAGE   4

struct expect {
    int kind;
    int64_t status;
    uint64_t vec, err, cr2;
    int cr2_kind, ripcr2, nohello, elf;
    int64_t ok, enosys, einval, efault;     /* -1: not checked */
};

static struct process sp;

static int starts(const char *s, const char *p)
{
    return memcmp(s, p, strlen(p)) == 0;
}

static int word_eq(const char *s, size_t n, const char *w)
{
    return strlen(w) == n && memcmp(s, w, n) == 0;
}

static uint64_t number(const char *s, size_t n, int *ok)
{
    uint64_t v = 0;
    int base = 10;

    if (n > 2 && s[0] == '0' && s[1] == 'x') {
        base = 16;
        s += 2;
        n -= 2;
    }
    if (n == 0)
        *ok = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
        if (d >= base)
            *ok = 0;
        v = v * base + (uint64_t)(d < base ? d : 0);
    }
    return v;
}

static int elf_code(const char *s, size_t n)
{
    static const struct { const char *name; int code; } names[] = {
        { "magic", ELF_EMAGIC }, { "dyn", ELF_EDYN }, { "wx", ELF_EWX }, { "entry", ELF_EENTRY },
        { "abi", ELF_EABI }, { "ptype", ELF_EPTYPE }, { "phbounds", ELF_EPHBOUNDS },
        { "overlap", ELF_EOVERLAP },
    };

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (word_eq(s, n, names[i].name))
            return names[i].code;
    return -1;
}

/* Parse "t-name key=value ..." into *e; returns 0 on a malformed line. */
static int parse(const char *cmd, char *name, struct expect *e)
{
    int ok = 1;
    const char *s = cmd;

    memset(e, 0, sizeof(*e));
    e->ok = e->enosys = e->einval = e->efault = -1;
    e->elf = -1;
    size_t n = 0;
    while (s[n] && s[n] != ' ')
        n++;
    if (n == 0 || n > PROC_NAME_MAX)
        return 0;
    memcpy(name, s, n);
    name[n] = 0;
    s += n;
    while (*s) {
        while (*s == ' ')
            s++;
        const char *k = s;
        size_t kl = 0, vl = 0;
        while (k[kl] && k[kl] != ' ' && k[kl] != '=')
            kl++;
        const char *v = k + kl + (k[kl] == '=');
        while (k[kl] == '=' && v[vl] && v[vl] != ' ')
            vl++;
        s = v + vl;
        if (kl == 0)
            continue;
        if (word_eq(k, kl, "expect"))
            e->kind = word_eq(v, vl, "exit") ? EXP_EXIT : word_eq(v, vl, "fault") ? EXP_FAULT :
                      word_eq(v, vl, "reject") ? EXP_REJECT : word_eq(v, vl, "umip") ? EXP_UMIP :
                      word_eq(v, vl, "smapac") ? EXP_SMAPAC : 0;
        else if (word_eq(k, kl, "status"))
            e->status = (int64_t)number(v, vl, &ok);
        else if (word_eq(k, kl, "vec"))
            e->vec = number(v, vl, &ok);
        else if (word_eq(k, kl, "err"))
            e->err = number(v, vl, &ok);
        else if (word_eq(k, kl, "cr2")) {
            e->cr2_kind = word_eq(v, vl, "stack") ? CR2_STACK : word_eq(v, vl, "guard") ? CR2_GUARD :
                          word_eq(v, vl, "image") ? CR2_IMAGE : CR2_EXACT;
            if (e->cr2_kind == CR2_EXACT)
                e->cr2 = number(v, vl, &ok);
        } else if (word_eq(k, kl, "ripcr2"))
            e->ripcr2 = 1;
        else if (word_eq(k, kl, "nohello"))
            e->nohello = 1;
        else if (word_eq(k, kl, "ok"))
            e->ok = (int64_t)number(v, vl, &ok);
        else if (word_eq(k, kl, "enosys"))
            e->enosys = (int64_t)number(v, vl, &ok);
        else if (word_eq(k, kl, "einval"))
            e->einval = (int64_t)number(v, vl, &ok);
        else if (word_eq(k, kl, "efault"))
            e->efault = (int64_t)number(v, vl, &ok);
        else if (word_eq(k, kl, "elf"))
            e->elf = elf_code(v, vl);
        else
            ok = 0;
    }
    if (e->kind == 0 || (e->kind == EXP_REJECT && e->elf < 0))
        ok = 0;
    return ok;
}

static int cr2_ok(const struct expect *e, uint64_t cr2)
{
    switch (e->cr2_kind) {
    case CR2_EXACT: return cr2 == e->cr2;
    case CR2_STACK: return cr2 >= USER_STACK_START && cr2 < USER_STACK_END;
    case CR2_GUARD: return cr2 >= USER_STACK_GUARD && cr2 < USER_STACK_START;
    case CR2_IMAGE: return cr2 >= USER_IMAGE_START && cr2 < USER_IMAGE_END;
    default: return 1;
    }
}

static int counters_ok(const struct expect *e, const struct process *p)
{
    return (e->ok < 0 || (uint64_t)e->ok == p->sys_ok) &&
           (e->enosys < 0 || (uint64_t)e->enosys == p->sys_enosys) &&
           (e->einval < 0 || (uint64_t)e->einval == p->sys_einval) &&
           (e->efault < 0 || (uint64_t)e->efault == p->sys_efault);
}

/* Kernel-side state that must hold after every case. */
static const char *invariants(const struct boot_info *bi, uint64_t f0)
{
    struct vmm_audit a;

    if (pmm_free_count() != f0)
        return "free frame count differs from the baseline";
    if (pmm_check_in(pmm_kernel()) != PMM_OK)
        return "allocator invariants broken";
    if (!gdt_stack_canaries_intact())
        return "kernel-entry stack canary overwritten";
    if ((read_cr3() & PTE_ADDR) != vmm_kernel_root() ||
        vmm_audit_space(vmm_kernel_root(), 1, &a) != VMM_OK || a.tables_lower != 0)
        return "kernel address space not intact";
    for (uint32_t i = 0; i < bi->module_count; i++)
        for (uint64_t pa = bi->modules[i].start & ~(PAGE_SIZE - 1); pa < bi->modules[i].end; pa += PAGE_SIZE)
            if (pmm_frame_state_in(pmm_kernel(), pa) != PMM_FRAME_UNAVAILABLE)
                return "a boot module frame is no longer reserved";
    if (*(volatile uint32_t *)(KERNEL_VMA + 0x100000) != 0xE85250D6u)
        return "kernel image bytes changed";
    return NULL;
}

static int run_hello(const struct boot_info *bi)
{
    const struct boot_module *m;
    struct elf_image im;
    int eerr;

    if (app_find_module(bi, &m) != 1 ||
        load_elf(&sp, "hello", phys_to_virt(m->start), m->end - m->start, &im, &eerr) != PROC_OK)
        return 0;
    int ok = proc_run(&sp) == PROC_EXITED && sp.exit_status == 0 && sp.sys_ok == 1 &&
             sp.trap.cs == GDT_USER_CS;
    proc_destroy(&sp);
    return ok;
}

int selftest_suite(const struct boot_info *bi)
{
    uint64_t f0 = pmm_free_count();
    unsigned cases = 0, failed = 0, hellos = 0;

    serial_puts("suite: CPU protections: smep=");
    serial_puts(read_cr4() & CR4_SMEP ? "on" : "off");
    serial_puts(" umip=");
    serial_puts(read_cr4() & CR4_UMIP ? "on" : "off");
    serial_puts(" smap=");
    serial_puts(read_cr4() & CR4_SMAP ? "on" : "off");
    serial_puts("\r\n");

    for (uint32_t i = 0; i < bi->module_count; i++) {
        const struct boot_module *m = &bi->modules[i];
        char name[PROC_NAME_MAX + 1];
        struct expect e;
        struct elf_image im;
        int eerr, pass = 0;
        const char *why = "";

        if (!starts(m->cmdline, "t-"))
            continue;
        cases++;
        if (!parse(m->cmdline, name, &e)) {
            why = "malformed expectation";
            goto verdict;
        }
        int r = load_elf(&sp, name, phys_to_virt(m->start), m->end - m->start, &im, &eerr);
        if (e.kind == EXP_REJECT) {
            pass = r == PROC_EEXEC && eerr == e.elf;
            why = pass ? elf_strerror(eerr) : r == PROC_EEXEC ? "rejected for another reason" : "accepted";
            goto checks;
        }
        if (r != PROC_OK) {
            why = r == PROC_EEXEC ? elf_strerror(eerr) : "could not be loaded";
            goto checks;
        }
        int st = proc_run(&sp);
        static struct process done;
        done = sp;
        proc_destroy(&sp);

        int kind = e.kind;
        if (kind == EXP_UMIP) {
            kind = (read_cr4() & CR4_UMIP) ? EXP_FAULT : EXP_EXIT;
            e.vec = 13;
            e.err = 0;
            e.status = 0;
        } else if (kind == EXP_SMAPAC) {
            kind = EXP_EXIT;
            e.status = cpu_smap_enabled ? 0 : 1;
        }
        if (done.trap.cs != GDT_USER_CS || done.trap.ss != GDT_USER_DS)
            why = "final frame not from CPL 3";
        else if (!counters_ok(&e, &done))
            why = "system-call results differ";
        else if (kind == EXP_EXIT && (st != PROC_EXITED || done.exit_status != e.status))
            why = st == PROC_EXITED ? "wrong exit status" : "killed";
        else if (kind == EXP_FAULT && (st != PROC_KILLED || done.trap.vector != e.vec ||
                                        done.trap.error != e.err))
            why = st == PROC_KILLED ? "wrong vector or error code" : "did not fault";
        else if (kind == EXP_FAULT && !cr2_ok(&e, done.trap.cr2))
            why = "wrong fault address";
        else if (kind == EXP_FAULT && e.ripcr2 && done.trap.rip != done.trap.cr2)
            why = "RIP is not the faulting fetch address";
        else if (kind == EXP_FAULT && !e.ripcr2 &&
                 (done.trap.rip < USER_IMAGE_START || done.trap.rip >= USER_IMAGE_END))
            why = "faulting RIP outside the program image";
        else {
            pass = 1;
            why = st == PROC_EXITED ? "exited" : done.reason;
        }
        serial_puts("suite: ");
        serial_puts(name);
        serial_puts(": cs=");
        serial_hex(done.trap.cs);
        serial_puts(" vec=");
        serial_dec(done.trap.vector);
        serial_puts(" err=");
        serial_hex(done.trap.error);
        serial_puts(" rip=");
        serial_hex(done.trap.rip);
        serial_puts(" cr2=");
        serial_hex(done.trap.cr2);
        serial_puts(" status=");
        serial_dec((uint32_t)done.exit_status);
        serial_puts(" ok/enosys/einval/efault=");
        serial_dec(done.sys_ok);
        serial_puts("/");
        serial_dec(done.sys_enosys);
        serial_puts("/");
        serial_dec(done.sys_einval);
        serial_puts("/");
        serial_dec(done.sys_efault);
        serial_puts("\r\n");
checks:
        if (pass) {
            const char *bad = invariants(bi, f0);
            if (bad) {
                pass = 0;
                why = bad;
            }
        }
        if (pass && !e.nohello) {
            pass = run_hello(bi) && !invariants(bi, f0);
            hellos++;
            if (!pass)
                why = "hello failed after this case";
        }
verdict:
        serial_puts("suite: ");
        serial_puts(m->cmdline);
        serial_puts(pass ? " => PASS (" : " => FAIL (");
        serial_puts(why ? why : "?");
        serial_puts(")\r\n");
        failed += !pass;
    }

    /* The acceptance run: hello once more, everything back to the baseline. */
    int final = run_hello(bi) && !invariants(bi, f0);
    serial_puts("suite: final hello ");
    serial_puts(final ? "exited 0, all frames reclaimed\r\n" : "FAILED\r\n");
    serial_puts("suite: ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failed);
    serial_puts(" failed, hello rerun ");
    serial_dec(hellos + 1);
    serial_puts(" times, free frames ");
    serial_dec(f0);
    serial_puts(" before and ");
    serial_dec(pmm_free_count());
    serial_puts(" after\r\n");
    return (int)failed + !final + (cases == 0);
}

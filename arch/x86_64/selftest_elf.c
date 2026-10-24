/*
 * ELF loading and execution self-tests (VOS_TEST only, N1.8).
 *
 * Uses the real GRUB module "hello". Expected values come from the module's
 * own bytes (read here independently of elf.c) and from constants fixed in
 * the test; execution evidence is CPU-generated (the saved frame).
 */
#include "selftest.h"
#include "app.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "kmap.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "vmm.h"

static int failures, cases;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

static uint64_t le(const uint8_t *p, uint64_t off, unsigned n)
{
    uint64_t v = 0;

    while (n--)
        v = v << 8 | p[off + n];
    return v;
}

static void put(uint8_t *p, uint64_t off, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; i++)
        p[off + i] = (uint8_t)(v >> (8 * i));
}

static struct process tp;
static uint8_t ebuf[32768] __attribute__((aligned(8)));

/* The real module and the facts the tests need, read independently of elf.c. */
static const uint8_t *img;
static uint64_t img_size, mod_start, mod_end, e_entry, phoff, phnum;
static uint64_t ph_text, ph_ro, ph_data, ph_note, ph_stack, note_off;

static uint64_t find_ph(uint32_t type, unsigned nth)
{
    for (uint64_t i = 0; i < phnum; i++)
        if (le(img, phoff + i * 56, 4) == type && nth-- == 0)
            return phoff + i * 56;
    return 0;
}

static int in_module(uint64_t pa)
{
    return pa + PAGE_SIZE > (mod_start & ~(PAGE_SIZE - 1)) && pa < mod_end;
}

/* ---- the run at boot (app.c) ---- */

static void test_boot_run(void)
{
    const struct app_result *r = app_last();

    expect(r->modules == 1 && r->ran && r->load_error == PROC_OK,
           "boot: exactly one module named hello was validated, loaded and run");
    expect(r->entry == e_entry && e_entry == 0x400000, "boot: execution began at the ELF header's entry");
    expect(r->state == PROC_EXITED && r->exit_status == 0, "boot: hello exited with status 0");
    expect(r->syscalls == 1, "boot: one system call returned to the process (debug_write)");
    expect(r->trap.vector == 0x80 && r->trap.cs == GDT_USER_CS && r->trap.ss == GDT_USER_DS,
           "boot: exit frame pushed by the CPU from CPL 3 (CS 0x23, SS 0x1B)");
    expect(r->trap.rip >= 0x400002 && r->trap.rip < 0x400000 + le(img, ph_text + 32, 8) &&
           le(img, le(img, ph_text + 8, 8) + (r->trap.rip - 2 - 0x400000), 2) == 0x80CD,
           "boot: exit RIP follows an int $0x80 inside hello's own code");
    expect(r->free_before == r->free_after, "boot: every frame returned after hello");
}

/* ---- load the real module again and inspect the address space ---- */

static unsigned expected_flags(uint32_t pflags)
{
    return VMM_USER | ((pflags & ELF_PF_W) ? VMM_WRITE : 0) | ((pflags & ELF_PF_X) ? VMM_EXEC : 0);
}

/* Check every page of every PT_LOAD of `image` against the process; returns mismatches. */
static unsigned check_segments(const uint8_t *image, const struct elf_image *im)
{
    unsigned bad = 0;
    struct vmm_mapping m;

    for (unsigned s = 0; s < im->nload; s++) {
        const struct elf_segment *g = &im->load[s];
        for (uint64_t off = 0; off < g->memsz; off += PAGE_SIZE) {
            if (vmm_lookup(tp.root, g->vaddr + off, &m) != VMM_OK || m.page_size != PAGE_SIZE ||
                m.flags != expected_flags(g->flags) || in_module(m.pa) ||
                pmm_frame_state_in(pmm_kernel(), m.pa) != PMM_FRAME_ALLOCATED) {
                bad++;
                continue;
            }
            const uint8_t *page = phys_to_virt(m.pa);
            for (uint64_t i = 0; i < PAGE_SIZE; i++) {
                uint8_t want = off + i < g->filesz ? image[g->offset + off + i] : 0;
                if (page[i] != want) {
                    bad++;
                    break;
                }
            }
        }
    }
    return bad;
}

static void test_load_and_run(void)
{
    uint64_t f0 = pmm_free_count();
    struct elf_image im;
    struct vmm_mapping m;
    struct vmm_audit a;
    int eerr;

    int r = load_elf(&tp, "hello", img, img_size, &im, &eerr);
    expect(r == PROC_OK && eerr == ELF_OK && im.nload == 3 && im.entry == e_entry,
           "the real module validates and loads (3 PT_LOAD)");
    if (r != PROC_OK)
        return;
    expect(check_segments(img, &im) == 0,
           "every page: exact permissions, file bytes then zeros, a fresh frame outside the module");
    expect(vmm_lookup(tp.root, 0x400000, &m) == VMM_OK && m.flags == (VMM_EXEC | VMM_USER) &&
           vmm_lookup(tp.root, 0x401000, &m) == VMM_OK && m.flags == VMM_USER &&
           vmm_lookup(tp.root, 0x402000, &m) == VMM_OK && m.flags == (VMM_WRITE | VMM_USER),
           "code R+X not writable, constants read-only and NX, data RW and NX");
    int stack_ok = 1;
    for (uint64_t va = USER_STACK_START; va < USER_STACK_END; va += PAGE_SIZE)
        stack_ok &= vmm_lookup(tp.root, va, &m) == VMM_OK && m.flags == (VMM_WRITE | VMM_USER);
    expect(stack_ok && vmm_lookup(tp.root, USER_STACK_GUARD, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(tp.root, 0, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(tp.root, 0x403000, &m) == VMM_ENOTMAPPED,
           "stack RW NX; guard page, null guard and the page after the image unmapped");
    int none_in_module = 1;
    for (unsigned i = 0; i < tp.npages; i++)
        none_in_module &= !in_module(tp.page_pa[i]);
    expect(none_in_module && tp.npages == im.pages + PROC_STACK_PAGES,
           "no user page maps a frame of the GRUB module");
    expect(vmm_audit_space(tp.root, 1, &a) == VMM_OK && a.user_leaves == tp.npages &&
           a.leaves_lower == tp.npages && a.large_upper == KMAP_LARGE_PAGES,
           "audit: only the process pages are user-accessible; kernel half supervisor-only");
    expect(vmm_lookup(tp.root, (uint64_t)phys_to_virt(mod_start), &m) == VMM_OK && !(m.flags & VMM_USER),
           "the module's kernel alias is supervisor-only in the process root");
    expect(pmm_free_count() == f0 - 1 - tp.npages - a.tables_lower,
           "frames used: root, image and stack pages, and the lower-half tables");

    expect(proc_run(&tp) == PROC_EXITED && tp.exit_status == 0 && tp.syscalls == 1,
           "the loaded hello runs in ring 3, writes once, exits 0");
    expect(tp.trap.cs == GDT_USER_CS && tp.trap.ss == GDT_USER_DS && tp.entry == e_entry,
           "ran at CPL 3 from the ELF entry point");
    proc_destroy(&tp);
    expect(pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK && gdt_stack_canaries_intact(),
           "all frames returned; allocator invariants and stack canaries intact");
    int module_reserved = 1;
    for (uint64_t pa = mod_start & ~(PAGE_SIZE - 1); pa < mod_end; pa += PAGE_SIZE)
        module_reserved &= pmm_frame_state_in(pmm_kernel(), pa) == PMM_FRAME_UNAVAILABLE;
    expect(module_reserved, "the GRUB module's frames stay reserved");
    expect(vmm_audit_space(vmm_kernel_root(), 1, &a) == VMM_OK && a.tables_lower == 0 &&
           (read_cr3() & PTE_ADDR) == vmm_kernel_root(), "kernel root intact and active");

    int again = 1;
    for (int i = 0; i < 10; i++) {
        again &= load_elf(&tp, "hello", img, img_size, &im, &eerr) == PROC_OK &&
                 proc_run(&tp) == PROC_EXITED && tp.exit_status == 0;
        proc_destroy(&tp);
    }
    expect(again && pmm_free_count() == f0, "10 more load/run/destroy cycles, no leak");

    /* A valid variant with a 3-page bss tail: the extra pages must be zero. */
    memcpy(ebuf, img, img_size);
    put(ebuf, ph_data + 40, 8, 0x2800);
    r = load_elf(&tp, "hello", ebuf, img_size, &im, &eerr);
    expect(r == PROC_OK && im.pages == 5 && check_segments(ebuf, &im) == 0,
           "segment larger than its file part: tail pages zero-filled");
    if (r == PROC_OK) {
        expect(proc_run(&tp) == PROC_EXITED && tp.exit_status == 0, "the variant runs too");
        proc_destroy(&tp);
    }

    /* Allocation failure at every step leaves nothing behind. */
    int rollback = 1;
    for (int b = 0; b < 1 + 3 + PROC_STACK_PAGES; b++) {
        proc_test_frame_budget(b);
        r = load_elf(&tp, "hello", img, img_size, &im, &eerr);
        proc_test_frame_budget(-1);
        rollback &= r == PROC_ENOMEM && pmm_free_count() == f0;
    }
    for (int b = 0; b < 6; b++) {
        vmm_test_table_budget(b);
        r = load_elf(&tp, "hello", img, img_size, &im, &eerr);
        vmm_test_table_budget(-1);
        rollback &= r == PROC_ENOMEM && pmm_free_count() == f0;
    }
    expect(rollback, "frame or table exhaustion at any step: PROC_ENOMEM, no frame leaked");
    expect(load_elf(&tp, "hello", img, img_size, &im, &eerr) == PROC_OK, "loading works afterwards");
    proc_destroy(&tp);
}

/* ---- corrupted executables: rejected for the intended reason, nothing allocated ---- */

struct mut { const char *name; int err; uint64_t size; unsigned n; uint64_t w[16][3]; };

static void test_rejections(void)
{
    uint64_t f0 = pmm_free_count();
    const uint64_t T = ph_text, R = ph_ro, D = ph_data, N = ph_note, S = ph_stack, NO = note_off;
    const uint64_t tfs = le(img, ph_text + 32, 8);
    static struct mut muts[80];
    unsigned k = 0;

#define M(nm, e, ...) muts[k++] = (struct mut){ nm, e, 0, sizeof((uint64_t[][3]){ __VA_ARGS__ }) / 24, { __VA_ARGS__ } }
#define SZ(nm, e, s) muts[k++] = (struct mut){ nm, e, s, 0, { { 0 } } }
    M("magic", ELF_EMAGIC, { 0, 1, 0x7E });
    M("class32", ELF_EIDENT, { 4, 1, 1 });
    M("big-endian", ELF_EIDENT, { 5, 1, 2 });
    M("ei-version", ELF_EIDENT, { 6, 1, 2 });
    M("osabi", ELF_EIDENT, { 7, 1, 3 });
    M("abiversion", ELF_EIDENT, { 8, 1, 1 });
    M("pad", ELF_EIDENT, { 9, 1, 1 });
    M("et-dyn", ELF_EDYN, { 16, 2, 3 });
    M("et-rel", ELF_ETYPE, { 16, 2, 1 });
    M("machine-i386", ELF_EMACHINE, { 18, 2, 3 });
    M("e-version", ELF_EHEADER, { 20, 4, 2 });
    M("e-flags", ELF_EHEADER, { 48, 4, 1 });
    M("ehsize", ELF_EHEADER, { 52, 2, 52 });
    M("phentsize", ELF_EHEADER, { 54, 2, 32 });
    M("phnum-0", ELF_EHEADER, { 56, 2, 0 });
    M("phnum-9", ELF_EHEADER, { 56, 2, 9 });
    M("phoff-in-header", ELF_EHEADER, { 32, 8, 8 });
    M("phoff-misaligned", ELF_EHEADER, { 32, 8, 68 });
    M("phoff-past-end", ELF_EPHBOUNDS, { 32, 8, img_size });
    M("phoff-huge", ELF_ERANGE, { 32, 8, 1ULL << 62 });
    M("phoff-wrap", ELF_ERANGE, { 32, 8, ~0ULL - 7 });
    M("entry-zero", ELF_EENTRY, { 24, 8, 0 });
    M("entry-rodata", ELF_EENTRY, { 24, 8, 0x401000 });
    M("entry-past-code", ELF_EENTRY, { 24, 8, 0x400000 + tfs });
    M("entry-kernel", ELF_ERANGE, { 24, 8, KERNEL_VMA });
    M("text-wx", ELF_EWX, { T + 4, 4, 7 });
    M("text-x-only", ELF_EFLAGS, { T + 4, 4, 1 });
    M("text-flags", ELF_EFLAGS, { T + 4, 4, 0x15 });
    M("no-exec", ELF_ENOEXEC, { T + 4, 4, 4 });
    M("data-wx", ELF_EWX, { D + 4, 4, 7 });
    M("align-2m", ELF_EALIGN, { T + 48, 8, 0x200000 });
    M("vaddr-unaligned", ELF_EALIGN, { T + 16, 8, 0x400010 });
    M("offset-unaligned", ELF_EALIGN, { T + 8, 8, 0x1004 });
    M("filesz-gt-memsz", ELF_ESEGSIZE, { D + 32, 8, 16 });
    M("memsz-zero", ELF_ESEGSIZE, { R + 32, 8, 0 }, { R + 40, 8, 0 });
    M("file-range", ELF_ESEGSIZE, { D + 32, 8, 0x10000 }, { D + 40, 8, 0x10000 });
    M("filesz-wrap", ELF_ERANGE, { D + 32, 8, ~0ULL }, { D + 40, 8, ~0ULL });
    M("offset-wrap", ELF_ERANGE, { D + 8, 8, ~0ULL - 0xFFF });
    M("vaddr-null", ELF_EUSERRANGE, { T + 16, 8, 0 });
    M("vaddr-low", ELF_EUSERRANGE, { T + 16, 8, 0x1000 });
    M("vaddr-high", ELF_EUSERRANGE, { D + 16, 8, 0x3FFFF000 }, { D + 40, 8, 0x2000 });
    M("vaddr-at-1gib", ELF_EUSERRANGE, { D + 16, 8, USER_IMAGE_END });
    M("vaddr-stack", ELF_ERANGE, { D + 16, 8, USER_STACK_START });  /* above 2^40 */
    M("vaddr-kernel", ELF_ERANGE, { D + 16, 8, KERNEL_VMA });
    M("memsz-near-limit", ELF_EUSERRANGE, { D + 40, 8, (1ULL << 40) - 1 });
    M("memsz-huge", ELF_ERANGE, { D + 40, 8, 1ULL << 63 });
    M("overlap", ELF_EOVERLAP, { R + 16, 8, 0x400000 });
    M("unsorted", ELF_EOVERLAP, { T + 16, 8, 0x403000 });
    M("image-over-1mib", ELF_EIMAGE, { D + 40, 8, 0x110000 });
    M("stack-exec", ELF_ESTACK, { S + 4, 4, 7 });
    M("interp", ELF_EPTYPE, { S, 4, 3 });
    M("dynamic", ELF_EPTYPE, { S, 4, 2 });
    M("tls", ELF_EPTYPE, { S, 4, 7 });
    M("phdr", ELF_EPTYPE, { S, 4, 6 });
    M("null", ELF_EPTYPE, { S, 4, 0 });
    M("relro", ELF_EPTYPE, { S, 4, 0x6474e552 });
    M("note-align", ELF_ENOTE, { N + 48, 8, 8 });
    M("note-outside", ELF_ENOTE, { N + 8, 8, img_size });
    M("note-trailing", ELF_ENOTE, { N + 32, 8, 24 });
    M("note-namesz-huge", ELF_ENOTE, { NO, 4, 0xFFFFFFFF });
    M("note-descsz", ELF_ENOTE, { NO + 4, 4, 8 });
    M("note-name", ELF_ENOTEMISSING, { NO + 12, 1, 'X' });
    M("note-type", ELF_ENOTEMISSING, { NO + 8, 4, 2 });
    M("note-version", ELF_EABI, { NO + 16, 4, 4 });   /* first unsupported version */
    /* A second VOS note in the file padding after the first, covered by PT_NOTE. */
    M("note-duplicate", ELF_ENOTEDUP, { N + 32, 8, 40 }, { NO + 20, 4, 4 }, { NO + 24, 4, 4 },
      { NO + 28, 4, 1 }, { NO + 32, 4, 0x00534F56 }, { NO + 36, 4, 0 });
    M("five-loads", ELF_ELOADCOUNT,
      { N, 4, 1 }, { N + 4, 4, 4 }, { N + 8, 8, 0x2000 }, { N + 16, 8, 0x404000 }, { N + 32, 8, 8 },
      { N + 40, 8, 8 }, { N + 48, 8, 0x1000 }, { S, 4, 1 }, { S + 4, 4, 4 }, { S + 8, 8, 0x2000 },
      { S + 16, 8, 0x405000 }, { S + 32, 8, 8 }, { S + 40, 8, 8 }, { S + 48, 8, 0x1000 });
    SZ("truncated-200", ELF_EPHBOUNDS, 200);
    SZ("truncated-40", ELF_ESIZE, 40);
    SZ("over-1mib", ELF_ESIZE, ELF_MAX_FILE + 1);
#undef M
#undef SZ

    unsigned ok = 0;
    for (unsigned i = 0; i < k; i++) {
        struct mut *u = &muts[i];
        struct elf_image im;
        int eerr;

        memcpy(ebuf, img, img_size);
        for (unsigned j = 0; j < u->n; j++)
            put(ebuf, u->w[j][0], (unsigned)u->w[j][1], u->w[j][2]);
        int r = load_elf(&tp, "neg", ebuf, u->size ? u->size : img_size, &im, &eerr);
        if (r == PROC_EEXEC && eerr == u->err && pmm_free_count() == f0) {
            ok++;
        } else {
            serial_puts("selftest:   loader mutation ");
            serial_puts(u->name);
            serial_puts(": got ");
            serial_puts(r == PROC_EEXEC ? elf_strerror(eerr) : "accepted or allocation error");
            serial_puts("\r\n");
            if (r == PROC_OK)
                proc_destroy(&tp);
        }
    }
    /* ABI v1 (N2.4): the same executable with note version 1 is accepted and recorded as v1. */
    struct elf_image im1;
    int e1;
    memcpy(ebuf, img, img_size);
    put(ebuf, NO + 16, 4, 1);
    int r1 = load_elf(&tp, "v1", ebuf, img_size, &im1, &e1);
    expect(r1 == PROC_OK && im1.abi == 1 && tp.abi == 1, "a VOS ABI note version 1 is accepted and recorded");
    if (r1 == PROC_OK)
        proc_destroy(&tp);
    put(ebuf, NO + 16, 4, 2);
    r1 = load_elf(&tp, "v2", ebuf, img_size, &im1, &e1);
    expect(r1 == PROC_OK && im1.abi == 2 && tp.abi == 2, "ABI note 2 accepted and recorded without changing entry state");
    if (r1 == PROC_OK)
        proc_destroy(&tp);
    expect(load_elf(&tp, "v0", img, img_size, &im1, &e1) == PROC_OK && im1.abi == 0 && tp.abi == 0,
           "hello's note version 0 is recorded as ABI v0");
    proc_destroy(&tp);
    expect(pmm_free_count() == f0, "the ABI-version loads leak nothing");

    serial_puts("selftest: ");
    serial_dec(ok);
    serial_puts("/");
    serial_dec(k);
    serial_puts(" corrupted executables rejected for the intended reason\r\n");
    expect(ok == k && k >= 60, "every corrupted executable rejected for its own reason, nothing allocated");
    struct elf_image im;
    int eerr;
    expect(load_elf(&tp, "hello", img, img_size, &im, &eerr) == PROC_OK, "no process left behind");
    proc_destroy(&tp);
    expect(pmm_free_count() == f0, "rejection tests leak nothing");
}

int selftest_elf(const struct boot_info *bi)
{
    const struct boot_module *m;

    failures = cases = 0;
    int n = app_find_module(bi, &m);
#if defined(VOS_TEST_CASE_modules) || defined(VOS_TEST_CASE_app_ambiguous) || defined(VOS_TEST_CASE_app_reject)
    const struct app_result *r = app_last();
#endif
#if defined(VOS_TEST_CASE_smep_exec) || defined(VOS_TEST_CASE_smap_touch)
    (void)n;
    goto out;                           /* these images test the CPU, not the loader */
#endif
#if defined(VOS_TEST_CASE_modules)
    expect(n == 0 && r->modules == 0 && !r->ran, "no hello module: nothing was run");
    goto out;
#elif defined(VOS_TEST_CASE_app_ambiguous)
    expect(n == 2 && r->modules == 2 && !r->ran && r->free_before == 0,
           "two hello modules: ambiguous, nothing loaded or run");
    goto out;
#elif defined(VOS_TEST_CASE_app_reject)
    expect(n == 1 && r->load_error == PROC_EEXEC && r->elf_error == ELF_EUSERRANGE && !r->ran &&
           r->free_after == r->free_before && pmm_free_count() == r->free_before,
           "kernel ELF as hello: rejected for its address range before any allocation");
    goto out;
#endif
    expect(n == 1, "exactly one module named hello");
    if (n != 1)
        goto out;
    img = phys_to_virt(m->start);
    img_size = m->end - m->start;
    mod_start = m->start;
    mod_end = m->end;
    expect(img_size <= sizeof(ebuf), "module fits the test buffer");
    if (img_size > sizeof(ebuf))
        goto out;
    e_entry = le(img, 24, 8);
    phoff = le(img, 32, 8);
    phnum = le(img, 56, 2);
    ph_text = find_ph(1, 0);
    ph_ro = find_ph(1, 1);
    ph_data = find_ph(1, 2);
    ph_note = find_ph(4, 0);
    ph_stack = find_ph(0x6474e551, 0);
    expect(ph_text && ph_ro && ph_data && ph_note && ph_stack, "module has the expected program headers");
    if (!(ph_text && ph_ro && ph_data && ph_note && ph_stack))
        goto out;
    note_off = le(img, ph_note + 8, 8);

    test_boot_run();
    test_load_and_run();
    test_rejections();
out:
    serial_puts("selftest: elf ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* USB.2-C3a hosted tests: the real PMM (with pmm_reserve), devmap, USB
 * resource and qualification/residue C code linked with the actual Rust
 * archive. An OVMF-like firmware residue lives in modeled RAM inside the
 * allocator's range. Each guard scenario runs in a forked child (the guard is
 * once per boot and its reservations are permanent by design). The model has
 * no write path: a PCI write aborts. C sanitizers do not instrument Rust. */
#ifndef VOS_CONTAIN_TEST
#error "this harness exercises the QEMU-only C3b.2 executor and needs -DVOS_CONTAIN_TEST"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define VOS_X86_64_CPU_H
#define VOS_X86_64_MEMLAYOUT_H
#define PAGE_SIZE 4096ULL
#define BOOT_MAP_LIMIT 0x40000000ULL
#define KERNEL_VMA 0xffffffff80000000ULL
#define RFLAGS_IF (1ULL << 9)
static unsigned char ram[2*1024*1024] __attribute__((aligned(4096)));
static uint64_t cr3, flags, pat = 0x0007040600070406ULL;
static int in_irq;
static void *phys_to_virt(uint64_t pa) { assert(pa < sizeof(ram)); return ram + pa; }
static uint64_t read_cr3(void) { return cr3; }
static uint64_t read_rflags(void) { return flags; }
static void invlpg(uint64_t va) { (void)va; }
static uint64_t rdmsr(unsigned msr) { (void)msr; return pat; }
static void wrmsr(unsigned msr,uint64_t v) { (void)msr; pat=v; }
static unsigned hv_bit = 1;
static uint32_t hv_sig[3] = {0x54474354, 0x43544743, 0x47435447};     /* "TCGTCGTCGTCG" */
static void cpuid(unsigned a,unsigned c,uint32_t *x,uint32_t *b,uint32_t *y,uint32_t *d)
{
    (void)c; *x=a==0x80000000?0x80000008:a==0x80000008?40:0; *b=0; *y=a==1?(uint32_t)hv_bit<<31:0; *d=1u<<16;
    if (a==0x40000000) { *b=hv_sig[0]; *y=hv_sig[1]; *d=hv_sig[2]; }
}
static uint32_t mmio_model(uint64_t va);
#define XHCI_QUALIFY_READ32(va, pa) mmio_model(va)
static void mmio_write(uint64_t va, uint32_t v);
#define XHCI_CONTAIN_WRITE32(va, v) mmio_write(va, v)
#include "../../arch/x86_64/pmm.c"
#include "../../arch/x86_64/devmap.c"
#include "../../arch/x86_64/usb_resources.c"
#include "../../arch/x86_64/xhci_qualify.c"
#include "../../arch/x86_64/resv.c"
#include "../../arch/x86_64/efi_memory.c"

static uint64_t root;
uint64_t vmm_kernel_root(void) { return root; }
int irq_in_handler(void) { return in_irq; }
int thread_state(uint64_t id) { (void)id; return THREAD_FREE; }
const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi,uint32_t i)
{ return (const void *)((const uint8_t *)bi->mmap+sizeof(*bi->mmap)+i*bi->mmap->entry_size); }
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr,"unexpected panic: %s\n",s); abort(); }

static char out[16384];
static size_t outlen;
static void emit(const char *s) { size_t n=strlen(s); assert(outlen+n<sizeof(out)); memcpy(out+outlen,s,n); outlen+=n; out[outlen]=0; }
void serial_puts(const char *s) { emit(s); }
void serial_hex(uint64_t v) { char b[24]; snprintf(b,sizeof b,"0x%llx",(unsigned long long)v); emit(b); }
void serial_dec(uint64_t v) { char b[24]; snprintf(b,sizeof b,"%llu",(unsigned long long)v); emit(b); }

/* ---- PCI and BAR models (as C1) ---- */
#define FUNCTIONS 3
static struct { int present; struct pci_addr a; uint32_t w[64]; } fn[FUNCTIONS];
static uint32_t bar[4096];
static unsigned change_after, mmio_reads, flip_at, w1_mode, w2_mode;   /* flip_at: USB.2-C3b.1 mid-session PORTSC change */
uint32_t pci_read32(struct pci_addr a, unsigned off)
{
    assert(off < 256 && !(off & 3));
    for (unsigned i=0;i<FUNCTIONS;i++)
        if (fn[i].present && !memcmp(&fn[i].a,&a,sizeof a)) return fn[i].w[off/4];
    return 0xffffffffu;
}
uint16_t pci_read16(struct pci_addr a, unsigned off) { return pci_read32(a,off&~3u)>>((off&2)*8); }
uint8_t pci_read8(struct pci_addr a, unsigned off) { return pci_read32(a,off&~3u)>>((off&3)*8); }
void pci_write8(struct pci_addr a, unsigned off, uint8_t v) { (void)a;(void)off;(void)v; abort(); }
struct wr { char kind; unsigned off; uint32_t val; };
static struct wr wlog[16];
static unsigned wn, session_writes, port_reads, port_flip_at, ports_at_w1;
/* Inside the executor's session (residue.session) every write happens with the freeze flag held: the guard's
 * reservation path refuses, so the session can neither reserve nor release a frame. Checked at each write. */
static void write_in_session(void)
{
    if (!residue.session) return;
    if (!residue.frozen || xhci_residue_reserve(0x1b0000, 1) != -4 || residue.count) {
        fprintf(stderr, "write made without the freeze flag held\n"); abort();
    }
    session_writes++;
}
/* The only authorized configuration write: the xHCI function's command register. */
void pci_write16(struct pci_addr a, unsigned off, uint16_t v)
{
    assert(wn < 16); wlog[wn++] = (struct wr){'P', off, v}; write_in_session(); ports_at_w1 = port_reads;
    if (memcmp(&a,&fn[0].a,sizeof a) || off != 4) { fprintf(stderr,"UNAUTHORIZED PCI write\n"); abort(); }
    if (w1_mode == 1) return;                                        /* the device ignores the write */
    uint32_t w = fn[0].w[1];
    fn[0].w[1] = (w & 0xffff0000u) | v;                               /* the 16-bit write leaves the status half alone */
    if (w1_mode == 2) fn[0].w[1] |= 0x00080000u;                      /* a status bit appears */
    if (w1_mode == 3) bar[0x44/4] |= 0x1000;                          /* HCE: a DMA attempt failed (QEMU xhci_die) */
    if (w1_mode == 4) fn[0].w[1] &= ~2u;                              /* memory decode lost as well */
}
void pci_write32(struct pci_addr a, unsigned off, uint32_t v) { (void)a;(void)off;(void)v; abort(); }
int pci_scan_bus0(int (*match)(struct pci_addr a, void *ctx), void *ctx, struct pci_addr *found)
{
    for (unsigned i=0;i<FUNCTIONS;i++)
        if (fn[i].present && match(fn[i].a,ctx)) { *found=fn[i].a; return 1; }
    return 0;
}
static uint32_t mmio_model(uint64_t va)
{
    uint64_t sva,len;
    assert(!devmap_scope_extent(devmap_scope_token(),&sva,&len) && va>=sva && va<sva+len && !(va&3));
    if (++mmio_reads==change_after) fn[0].w[1]=0x00100103;   /* another agent clears BME */
    if (flip_at && mmio_reads==flip_at) bar[(0x440+16*7)/4]=0x2a1;  /* a device connects on the last port */
    unsigned off = va - sva;
    if (off >= 0x440 && off < 0x440 + 128 && ++port_reads == port_flip_at) bar[off/4] = 0x2a1;   /* a device on THIS port read */
    return bar[off/4];
}
/* The only authorized register write: USBCMD. HCRST runs QEMU's xhci_reset (synchronous). */
static void mmio_write(uint64_t va, uint32_t v)
{
    uint64_t sva,len;
    assert(!devmap_scope_extent(devmap_scope_token(),&sva,&len) && va>=sva && va<sva+len && !(va&3));
    unsigned off = va - sva;
    assert(wn < 16); wlog[wn++] = (struct wr){'M', off, v}; write_in_session();
    if (off != 0x40) { fprintf(stderr,"UNAUTHORIZED register write %#x\n", off); abort(); }
    bar[off/4] = v;
    if (!(v & 2) || w2_mode == 1) return;                             /* mode 1: HCRST never clears */
    bar[0x40/4] = 0; bar[0x44/4] = w2_mode == 2 ? 0x801 : 1;          /* mode 2: CNR stays set */
    bar[0x54/4] = 0; bar[0x58/4] = bar[0x5c/4] = 0; bar[0x70/4] = bar[0x74/4] = 0; bar[0x78/4] = 0;
    for (unsigned n = 0; n < 16; n++) {
        unsigned at = (0x1020 + 32*n) / 4;
        for (unsigned k = 0; k < 8; k++) bar[at+k] = 0;
        if (w2_mode == 6) bar[at+1] = 4000;                           /* IMOD at the specification default */
    }
    for (unsigned p = 0; p < 8; p++) bar[(0x440+16*p)/4] = 0x2a0 | 1u << 17;   /* QEMU: CSC on every port */
    if (w2_mode == 3) bar[0x1038/4] = 0x1a0000;                       /* an event ring pointer survives */
    if (w2_mode == 4) bar[0x58/4] = 0x1a0240;                         /* the command ring pointer survives */
    if (w2_mode == 5) bar[0x440/4] |= 1;                              /* a port reports a device */
}

/* OVMF-like residue in modeled RAM: DCBAA 0x1a0000 (64 slots), command ring
 * 0x1a0240 (256 TRBs, Link back to start), event segment 0x1a1240 (512 TRBs),
 * ERST 0x1a3240; interrupter 0 IE set; controller halted. */
#define DCBAA 0x1a0000ULL
#define RING  0x1a0240ULL
#define SEG   0x1a1240ULL
#define ERST  0x1a3240ULL
static void put64(uint64_t pa, uint64_t v) { memcpy(phys_to_virt(pa),&v,8); }
static void model(void)
{
    memset(fn,0,sizeof fn); memset(bar,0,sizeof bar);
    fn[0].present=1; fn[0].a=(struct pci_addr){0,2,0};
    uint32_t *w=fn[0].w;
    w[0]=0x000d1b36; w[1]=0x00100107; w[2]=0x0c033001; w[4]=0xfebf0004;
    w[0x34/4]=0x90; w[0x90/4]=0x000fa011; w[0x94/4]=0x3000; w[0x98/4]=0x3800; w[0xa0/4]=0x00020010;
    bar[0]=0x01000040; bar[1]=0x08001040; bar[2]=0xf; bar[4]=0x00087001; bar[5]=0x2000; bar[6]=0x1000;
    bar[0x20/4]=0x02000402; bar[0x24/4]=0x20425355; bar[0x28/4]=0x0401;
    bar[0x30/4]=0x03000002; bar[0x34/4]=0x20425355; bar[0x38/4]=0x0405;
    bar[0x44/4]=1; bar[0x48/4]=1;                           /* USBSTS.HCH, PAGESIZE */
    bar[(0x40+0x18)/4]=(uint32_t)RING|1; bar[(0x40+0x30)/4]=(uint32_t)DCBAA; bar[(0x40+0x38)/4]=64;
    bar[0x1020/4]=2; bar[0x1028/4]=1; bar[0x1030/4]=(uint32_t)ERST; bar[0x1038/4]=(uint32_t)SEG;
    memset(ram+0x1a0000,0,0x8000);
    put64(RING+255*16,RING); put64(RING+255*16+8,(uint64_t)(6u<<10|2)<<32);
    put64(ERST,SEG); put64(ERST+8,512);
    for (unsigned p=0;p<8;p++) bar[(0x440+16*p)/4]=0x2a0;      /* idle PORTSC: PP, RxDetect */
    change_after=0; mmio_reads=0; flip_at=0; outlen=0; out[0]=0;
}

static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n%s\n",__FILE__,__LINE__,#x,out);_exit(1);} } while(0)
static struct { struct mb2_tag_mmap header; struct mb2_mmap_entry entries[3]; } mm = {
    .header={.entry_size=sizeof(struct mb2_mmap_entry)},
    .entries={{.base=0x100000,.length=0x100000,.type=MB2_MMAP_AVAILABLE},
              {.base=0x100000000ULL,.length=0x4000,.type=2},
              {.base=0x1a0000,.length=0,.type=2}}   /* length set to protect the residue */
};
static struct boot_info bi={.mmap=&mm.header,.mmap_entries=3};
static uint64_t base;
static void setup(void)
{
    CHECK(pmm_init(&bi,NULL)==0);
    uint64_t pdpt;
    CHECK(pmm_alloc_frame(&root)==0); CHECK(pmm_alloc_frame(&pdpt)==0);
    ((uint64_t *)phys_to_virt(root))[511]=pdpt|PTE_P|PTE_W;
    cr3=root; CHECK(devmap_init(&bi)==0);
    model();
    base=pmm_free_count();
}

static void scenario(const char *name, void (*body)(void))
{
    fflush(stdout);
    pid_t pid=fork();
    if (!pid) { setup(); body(); printf("  %s: %u checks PASS\n",name,checks); fflush(stdout); _exit(0); }
    int status=0; assert(waitpid(pid,&status,0)==pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr,"scenario %s FAILED\n",name); exit(1); }
}

/* ---- USB.2-C3a.3: C3a.2-excluded firmware RAM, inspected in place ---- */
static struct { struct mb2_tag_mmap header; struct mb2_mmap_entry entries[3]; } em = {
    .header={.tag={MB2_TAG_MMAP,sizeof(em)},.entry_size=sizeof(struct mb2_mmap_entry)},
    .entries={{.base=0x100000,.length=0x100000,.type=MB2_MMAP_AVAILABLE},
              {.base=0x100000000ULL,.length=0x4000,.type=2},
              {.base=0x1a0800,.length=0,.type=2}}   /* length set to touch the residue */
};
static uint8_t efi17[16+3*48] __attribute__((aligned(8)));
static struct resv_table et;
static struct boot_info ebi;
static void desc(int i, uint32_t type, uint64_t start, uint64_t pages)
{
    uint8_t *d=efi17+16+48*i; uint64_t wb=8;
    memset(d,0,48); memcpy(d,&type,4); memcpy(d+8,&start,8); memcpy(d+24,&pages,8); memcpy(d+32,&wb,8);
}
/* OVMF-like: boot-services data over [0x1a0000, +bs_pages), conventional around it. */
static void efi_boot(unsigned bs_pages, int exclude)
{
    uint32_t h[4]={MB2_TAG_EFI_MMAP,sizeof efi17,48,1};
    memcpy(efi17,h,16);
    desc(0,7,0x100000,0xa0); desc(1,4,0x1a0000,bs_pages); desc(2,7,0x1a0000+bs_pages*4096ULL,0x60-bs_pages);
    ebi=(struct boot_info){.mmap=&em.header,.mmap_entries=3,.efi=1,.efi_mmap=(const void *)efi17,.efi_mmap_tags=1};
    resv_init(&et);
    struct efi_memory_report rep;
    if (exclude) CHECK(efi_memory_exclude(&ebi,&et,&rep)==EFI_MEMORY_APPLIED && rep.frames>=bs_pages);
    kernel_pmm.state=PMM_UNINIT; CHECK(pmm_init(&ebi,&et)==0);
    CHECK(pmm_alloc_frame(&root)==0); uint64_t pdpt; CHECK(pmm_alloc_frame(&pdpt)==0);
    ((uint64_t *)phys_to_virt(root))[511]=pdpt|PTE_P|PTE_W; cr3=root;
    memset(&scope,0,sizeof scope); count=0; ready=0; CHECK(devmap_init(&ebi)==0);
    model(); base=pmm_free_count();
}

/* ---- USB.2-C3b.2: the executor against the same models ---- */
static uint32_t pci_snap[FUNCTIONS][64], bar_snap[4096];
static uint8_t ram_snap[0x8000];
static int exec_run(const char *opts) { outlen=0; out[0]=0; wn=0; return xhci_contain_execute_boot(opts); }
#define EXEC "vos.usb2-contain-execute=1"
static void put_info(const char *oem)
{
    uint8_t *p = ram + 0x1fe000; memset(p, 0, 64);
    uint32_t total = 48, hdr[2] = {14, 28};
    memcpy(p, &total, 4); memcpy(p+8, hdr, 8);
    memcpy(p+16, "RSD PTR ", 8); memcpy(p+16+9, oem, 6);
    uint32_t endtag[2] = {0, 8}; memcpy(p+8+32, endtag, 8);
}
/* The eligible QEMU profile: UEFI idle controller, command 0x0007, the real subsystem and host bridge. */
static void exec_setup(void)
{
    efi_boot(8,1);
    fn[0].w[1] = 0x00100007; fn[0].w[0x2c/4] = 0x11001af4;
    fn[2].present = 1; fn[2].a = (struct pci_addr){0,0,0};
    fn[2].w[0] = 0x29c08086; fn[2].w[2] = 0x06000000; fn[2].w[0x2c/4] = 0x11001af4;
    hv_bit = 1; hv_sig[0] = 0x54474354; hv_sig[1] = 0x43544743; hv_sig[2] = 0x47435447;
    put_info("BOCHS "); ebi.info_start = 0x1fe000; ebi.info_end = 0x1fe030;
    w1_mode = w2_mode = 0; wn = 0;
    xhci_residue_boot_memory(&ebi,&et);
}
static void snap_all(void)
{
    for (unsigned i=0;i<FUNCTIONS;i++) memcpy(pci_snap[i],fn[i].w,sizeof pci_snap[i]);
    memcpy(bar_snap,bar,sizeof bar); memcpy(ram_snap,ram+0x1a0000,sizeof ram_snap);
}
/* After any outcome: nothing leased, mapped, reserved or left frozen; firmware memory untouched. */
static void exec_after(void)
{
    CHECK(!residue.frozen && !residue.session && !residue.bi && !residue.resv && !residue.count && !exe.armed);
    CHECK(pmm_free_count()==base && pmm_check_in(pmm_kernel())==PMM_OK);
    CHECK(!devmap_scope_token() && !qual.token && !devmap_audit() && !qual.active);
    CHECK(!memcmp(ram_snap, ram+0x1a0000, sizeof ram_snap));
}
/* Not requested: the boot function returns at once; the C3a.3 boot-info pointers (used only inside a session) stay set. */
static void exec_after_idle(void)
{
    CHECK(!residue.frozen && !residue.session && !residue.count && !exe.armed && !exe.ran && !exe.w1 && !exe.w2);
    CHECK(pmm_free_count()==base && pmm_check_in(pmm_kernel())==PMM_OK && !devmap_scope_token() && !qual.token && !qual.active);
    CHECK(!memcmp(ram_snap, ram+0x1a0000, sizeof ram_snap));
}
/* Refused before any session started (conflicting option): the same, and the single attempt is counted. */
static void exec_after_early(void)
{
    CHECK(!residue.frozen && !residue.session && !residue.count && !exe.armed && exe.ran==1 && !exe.w1 && !exe.w2);
    CHECK(pmm_free_count()==base && pmm_check_in(pmm_kernel())==PMM_OK && !devmap_scope_token() && !qual.token && !qual.active);
    CHECK(!memcmp(ram_snap, ram+0x1a0000, sizeof ram_snap));
}
static void no_device_change(void)
{
    for (unsigned i=0;i<FUNCTIONS;i++) CHECK(!memcmp(pci_snap[i],fn[i].w,sizeof pci_snap[i]));
    CHECK(!memcmp(bar,bar_snap,sizeof bar) && wn==0);
}
static const char *CONTAINED = "usb2c3b2: CONTAINED: W1 and W2 issued and verified; bus mastering off, controller reset and halted";

static void x_eligible(void)
{
    exec_setup(); snap_all();
    CHECK(xhci_contain_execute_boot("")==1);                           /* disabled by default, even in the test image */
    CHECK(strstr(out,"VOS-CONTAIN-TEST-IMAGE: QEMU-ONLY EXPERIMENTAL; NEVER FOR PHYSICAL PCS") && !strstr(out,"usb2c3b2: CONTAINED"));
    no_device_change();
    CHECK(exec_run(EXEC)==1 && strstr(out,CONTAINED) && !strstr(out,"FAIL"));
    CHECK(wn==2 && wlog[0].kind=='P' && wlog[0].off==4 && wlog[0].val==0x0003);
    CHECK(wlog[1].kind=='M' && wlog[1].off==0x40 && wlog[1].val==0x2);
    CHECK(session_writes==2);                                           /* both writes were made with the freeze flag held */
    CHECK(fn[0].w[1]==0x00100003);                                      /* only Bus Master Enable changed; status intact */
    CHECK(fn[1].present==0 && !memcmp(pci_snap[2],fn[2].w,sizeof pci_snap[2]));
    CHECK(bar[0x40/4]==0 && bar[0x44/4]==1 && bar[0x58/4]==0 && bar[0x70/4]==0 && bar[0x78/4]==0);
    for (unsigned p=0;p<8;p++) CHECK(bar[(0x440+16*p)/4]==(0x2a0u|1u<<17));   /* QEMU's CSC on every port is accepted */
    CHECK(strstr(out,"state=0xd") && strstr(out,"writes=0x2") && strstr(out,"polls=0x1") && strstr(out,"refusals=0x0"));
    CHECK(strstr(out,"w1_old=0x7") && strstr(out,"w1_new=0x3") && strstr(out,"w1_readback=0x3") && strstr(out,"w1_status=0x10"));
    CHECK(strstr(out,"usbsts_w1=0x1") && strstr(out,"w2_old=0x0") && strstr(out,"w2_new=0x2"));
    CHECK(strstr(out,"usbcmd=0x0 usbsts=0x1") && strstr(out,"ports_active=0x0") && strstr(out,"irq_regs_clear=0x1"));
    CHECK(strstr(out,"crcr=0x0 dcbaap=0x0") && strstr(out,"command=0x3 status=0x10"));
    exec_after();
    unsigned n = wn; CHECK(exec_run(EXEC)==0 && wn==0 && n==2);         /* once per boot: nothing more, ever */
}
static void x_disabled_by_default(void)
{
    exec_setup(); snap_all();
    CHECK(exec_run("")==1 && wn==0); CHECK(exec_run("vos.usb2-contain=1")==1 && wn==0);
    CHECK(exec_run("vos.usb2-contain-execute=0")==1 && wn==0 && !strstr(out,"REFUSED"));
    CHECK(exec_run("vos.usb2-contain-execute=10")==1 && wn==0);
    CHECK(exec_run("xvos.usb2-contain-execute=1 vos.usb2-contain-execute=1x")==1 && wn==0);   /* exact tokens only */
    no_device_change(); exec_after_idle();
}
#define X_CONFLICT(fname, option) static void fname(void) { exec_setup(); snap_all(); \
    CHECK(exec_run(EXEC " " option)==1 && strstr(out,"usb2c3b2: REFUSED: conflicting option " option "; nothing written")); \
    no_device_change(); exec_after_early(); CHECK(!strstr(out,"CONTAINED")); }
X_CONFLICT(x_c_contain,  "vos.usb2-contain=1")
X_CONFLICT(x_c_residue,  "vos.usb2-residue=1")
X_CONFLICT(x_c_qualify,  "vos.usb2-qualify=1")
X_CONFLICT(x_c_dma,      "vos.usb2c2-dma=1")
X_CONFLICT(x_c_probe,    "vos.usb_probe=1")
#define X_REFUSED(fname, edit, reason) static void fname(void) { exec_setup(); edit; snap_all(); \
    CHECK(exec_run(EXEC)==1 && strstr(out,"usb2c3b2: REFUSED:") && strstr(out,reason) && strstr(out,"; nothing written")); \
    CHECK(!strstr(out,"CONTAINED")); no_device_change(); exec_after(); }
X_REFUSED(x_bios,       (ebi.efi=0),                                   " not-uefi")
X_REFUSED(x_cmd,        (fn[0].w[1]=0x00100107),                       " pci-command")
X_REFUSED(x_running,    (bar[0x40/4]=1, bar[0x44/4]=0),                " running")
X_REFUSED(x_eint,       (bar[0x44/4]=9),                               " usbsts")
X_REFUSED(x_inte,       (bar[0x40/4]=4),                               " interrupts")
X_REFUSED(x_ip,         (bar[0x1020/4]=3),                             " interrupts")
X_REFUSED(x_msix,       (fn[0].w[0x90/4]|=0x80000000u),                " interrupts")
X_REFUSED(x_slot,       (put64(DCBAA+8,0x1c0000)),                     " slots")
X_REFUSED(x_port,       (bar[0x450/4]=0x202a0),                        " ports")
X_REFUSED(x_connected,  (bar[0x440/4]=0x2a1),                          " ports")
X_REFUSED(x_pcie_busy,  (fn[0].w[0xa8/4]=1u<<21),                      " pcie-busy")
X_REFUSED(x_profile,    (bar[4]=0x00087005),                           " profile")
X_REFUSED(x_nec,        (fn[0].w[0]=0x01941033),                       " unqualified")
X_REFUSED(x_none,       (fn[0].present=0),                             " unqualified")
X_REFUSED(x_two,        (fn[1]=fn[0], fn[1].a.dev=3),                  " unqualified")
X_REFUSED(x_context,    (flags=RFLAGS_IF),                             " unqualified")
X_REFUSED(x_subsystem,  (fn[0].w[0x2c/4]=0x11001af5),                  " subsystem")
X_REFUSED(x_subsystem0, (fn[0].w[0x2c/4]=0),                           " subsystem")
X_REFUSED(x_no_hv,      (hv_bit=0),                                    " environment")
X_REFUSED(x_kvm_sig,    (hv_sig[0]=0x4b4d564b, hv_sig[1]=0x4b4d564b, hv_sig[2]=0x4d),   " environment")
X_REFUSED(x_sig_bit,    (hv_sig[2]^=1),                                " environment")
X_REFUSED(x_oem,        (put_info("EVILRS")),                          " environment")
X_REFUSED(x_no_acpi,    (ebi.info_start=0, ebi.info_end=0),            " environment")
X_REFUSED(x_hostbridge, (fn[2].w[0]=0x12378086),                       " environment")
X_REFUSED(x_hostbr_sub, (fn[2].w[0x2c/4]=0),                           " environment")
X_REFUSED(x_no_hostbr,  (fn[2].present=0),                             " environment")

/* The device misbehaves after W1: every outcome is final, W2 is never issued, nothing else is written. */
#define X_W1(fname, mode, state_text) static void fname(void) { exec_setup(); w1_mode = mode; snap_all(); \
    CHECK(exec_run(EXEC)==1 && strstr(out,"usb2c3b2: STOPPED: state " state_text "; writes final, bus mastering stays off")); \
    CHECK(wn==1 && wlog[0].kind=='P' && wlog[0].val==3 && !strstr(out,"CONTAINED")); \
    exec_after(); }
X_W1(x_w1_ignored, 1, "9 bus-master-stuck")
X_W1(x_w1_status,  2, "17 PCI-changed")
/* C1's own end-of-session comparison also sees USBSTS change: a loud integrity failure, naming the state. */
static void x_w1_hce(void)
{
    exec_setup(); w1_mode = 3; snap_all();
    CHECK(exec_run(EXEC)==0 && strstr(out,"usb2c3b2: FAIL: integrity") && strstr(out,"state 18 controller-error; bus mastering stays off"));
    CHECK(strstr(out,"state=0x12") && !strstr(out,"CONTAINED") && wn==1 && wlog[0].kind=='P' && wlog[0].val==3); exec_after();
}
/* More than BME changed: C1's own PCI re-observation also catches it (integrity failure), after W1 only. */
static void x_w1_more(void)
{
    exec_setup(); w1_mode = 4; snap_all();
    CHECK(exec_run(EXEC)==0 && strstr(out,"usb2c3b2: FAIL: integrity") && strstr(out,"state 17 PCI-changed; bus mastering stays off"));
    CHECK(strstr(out,"state=0x11") && !strstr(out,"CONTAINED"));
    CHECK(wn==1 && wlog[0].kind=='P' && wlog[0].val==3); exec_after();
}
static void x_bme_never_set(void)
{
    exec_setup(); w1_mode = 1; snap_all();
    exec_run(EXEC);
    for (unsigned i=0;i<wn;i++) CHECK(!(wlog[i].kind=='P' && (wlog[i].val & 4)));
    CHECK(fn[0].w[1] == 0x00100007);                                    /* the device ignored W1: VOS did nothing more */
}
/* The reset misbehaves after W2: both writes were made, no third write, bounded reads. */
#define X_W2(fname, mode, state_text) static void fname(void) { exec_setup(); w2_mode = mode; snap_all(); mmio_reads = 0; \
    CHECK(exec_run(EXEC)==1 && strstr(out,"usb2c3b2: STOPPED: state " state_text "; writes final, bus mastering stays off")); \
    CHECK(wn==2 && wlog[0].kind=='P' && wlog[1].kind=='M' && !strstr(out,"CONTAINED") && fn[0].w[1]==0x00100003); \
    CHECK(mmio_reads < 4000); exec_after(); }
/* A stuck reset leaves USBCMD/USBSTS different from the start of the session, which C1's own end-of-session
 * comparison also flags: a loud integrity failure naming the state. Both writes were made; nothing more is. */
#define X_W2F(fname, mode, state_text) static void fname(void) { exec_setup(); w2_mode = mode; snap_all(); mmio_reads = 0; \
    CHECK(exec_run(EXEC)==0 && strstr(out,"usb2c3b2: FAIL: integrity") && strstr(out,"; state " state_text "; bus mastering stays off")); \
    CHECK(wn==2 && wlog[0].kind=='P' && wlog[1].kind=='M' && !strstr(out,"CONTAINED") && fn[0].w[1]==0x00100003); \
    CHECK(mmio_reads < 4000); exec_after(); }
X_W2F(x_w2_stuck,   1, "11 reset-stuck")
X_W2F(x_w2_cnr,     2, "11 reset-stuck")
X_W2(x_w2_erdp,     3, "12 reset-unverified")
X_W2(x_w2_crcr,     4, "12 reset-unverified")
X_W2(x_w2_port,     5, "12 reset-unverified")
static void x_w2_imod(void)
{
    exec_setup(); w2_mode = 6; snap_all();
    CHECK(exec_run(EXEC)==1 && strstr(out,CONTAINED) && wn==2);        /* IMOD at its specification default is accepted */
    CHECK(bar[0x1024/4]==4000); exec_after();
}
static void x_stuck_polls_bounded(void)
{
    exec_setup(); w2_mode = 1; snap_all(); mmio_reads = 0;
    CHECK(exec_run(EXEC)==0 && strstr(out,"polls=0x400"));              /* exactly POLL_BOUND, then final */
    CHECK(wn==2 && bar[0x40/4]==2);                                     /* HCRST left alone, never aborted or retried */
    exec_after();
}

/* Drift sweep: a device appears at every possible read of a full execution. The writes made are always a
 * valid prefix (none, W1, or W1 then W2), W2 never without a verified W1, and bus mastering ends off. */
/* A device appears on a specific PORTSC read. Every port read of an execution is guarded by exactly one stage,
 * and which one fires is pinned: before W1 nothing is written; the post-W1 pass stops at contained-bme-only; the
 * immediate pre-W2 re-read stops at pre-W2-drift; the post-reset check ends unverified. */
static void x_ports_guarded(void)
{
    exec_setup(); snap_all(); port_reads = 0;
    CHECK(exec_run(EXEC)==1 && strstr(out,CONTAINED));
    unsigned before = ports_at_w1, total = port_reads;
    CHECK(before > 0 && total == before + 24);                         /* pass 5 (8) + pre-W2 re-read (8) + post-reset (8) */
    for (unsigned n = 1; n <= total + 1; n++) {
        exe.ran = 0; exe.w1 = exe.w2 = 0;
        exec_setup(); memcpy(ram_snap, ram+0x1a0000, sizeof ram_snap); port_reads = 0; port_flip_at = n;
        int ret = exec_run(EXEC);
        CHECK(ret == 1);
        if (n <= before)             CHECK(wn == 0 && strstr(out, "nothing written"));
        else if (n <= before + 8)    CHECK(wn == 1 && strstr(out, "state 10 contained-bme-only; writes final"));
        else if (n <= before + 16)   CHECK(wn == 1 && strstr(out, "state 19 pre-W2-drift; writes final"));
        else if (n <= before + 24)   CHECK(wn == 2 && strstr(out, "state 12 reset-unverified; writes final"));
        else                         CHECK(wn == 2 && strstr(out, CONTAINED));
        CHECK(fn[0].w[1] == (wn ? 0x00100003u : 0x00100007u) && !residue.frozen && !residue.count);
    }
    port_flip_at = 0;
}
static void x_sweep(void)
{
    exec_setup(); snap_all(); mmio_reads = 0;
    CHECK(exec_run(EXEC)==1 && strstr(out,CONTAINED));
    unsigned total = mmio_reads, classes[3] = {0,0,0};
    for (unsigned k = 1; k <= total + 2; k++) {
        exe.ran = 0; exe.w1 = exe.w2 = 0;
        model(); exec_setup(); memcpy(ram_snap, ram+0x1a0000, sizeof ram_snap); flip_at = k; mmio_reads = 0;
        CHECK(exec_run(EXEC)==1);
        CHECK(wn<=2 && (wn<1 || (wlog[0].kind=='P' && wlog[0].val==3 && wlog[0].off==4)) &&
              (wn<2 || (wlog[1].kind=='M' && wlog[1].off==0x40 && wlog[1].val==2)));
        CHECK(wn==0 ? (fn[0].w[1]==0x00100007 && strstr(out,"nothing written")) : (fn[0].w[1]==0x00100003));
        CHECK(wn!=2 || bar[0x40/4]==0);                                 /* W2 only ever completed its reset */
        classes[wn]++;
        CHECK(!residue.frozen && !residue.count && pmm_free_count()==base && !memcmp(ram_snap, ram+0x1a0000, sizeof ram_snap));
    }
    CHECK(classes[0] > 0 && classes[1] > 0 && classes[2] > 0);          /* all three outcome classes are reached */
    CHECK(classes[0] + classes[1] + classes[2] == total + 2);
}

/* The adapters themselves: each refuses unless every one of its own preconditions holds. */
static uint64_t token;
static void adapter_setup(void)
{
    exec_setup(); qual.fn = fn[0].a; qual.active = 1; exe.armed = 1; flags = 0; cr3 = root;
    struct usb_resource r; CHECK(xhci_qualify_bar_acquire(0xfebf0000, &r)==0); token = r.token;
    wn = 0; snap_all();
}
#define A1(old, value) xhci_contain_w1(old, value)
static void a_w1(void)
{
    adapter_setup();
    exe.armed = 0; CHECK(A1(7,3)!=0 && wn==0); exe.armed = 1;                 /* not armed */
    qual.active = 0; CHECK(A1(7,3)!=0 && wn==0); qual.active = 1;            /* outside a session */
    flags = RFLAGS_IF; CHECK(A1(7,3)!=0 && wn==0); flags = 0;                 /* interrupts enabled */
    CHECK(A1(3,3)!=0 && wn==0);                                               /* BME already clear in `old` (device holds 7) */
    fn[0].w[1] = 0x00100003;                                                  /* ... and with the device really holding it clear */
    CHECK(A1(3,3)!=0 && wn==0); CHECK(A1(0x0003,0x0003)!=0 && wn==0);        /* no pointless or repeated write either */
    fn[0].w[1] = 0x00100007;
    CHECK(A1(7,1)!=0 && wn==0 && A1(7,7)!=0 && wn==0 && A1(7,0x0403)!=0 && wn==0);   /* anything but old & ~BME */
    CHECK(A1(0x0107,0x0103)!=0 && wn==0);                                     /* `old` is not what the device holds */
    CHECK(A1(7,3)==0 && wn==1 && wlog[0].kind=='P' && wlog[0].off==4 && wlog[0].val==3 && fn[0].w[1]==0x00100003);
    CHECK(A1(7,3)!=0 && wn==1);                                               /* once */
    fn[0].w[1] = 0x00100007;                                                  /* the state looks eligible again ... */
    CHECK(A1(7,3)!=0 && wn==1 && fn[0].w[1]==0x00100007);                     /* ... but W1 is spent for the whole boot */
}
static void a_w2_order(void)
{
    adapter_setup();
    CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==0);                       /* never before W1 */
    fn[0].w[1] = 0x00100003;                                                  /* bus mastering already off, but VOS never issued W1 */
    CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==0 && bar[0x40/4]==0);
    fn[0].w[1] = 0x00100007;
    CHECK(A1(7,3)==0 && wn==1);
    CHECK(xhci_contain_w2(token+1,0x40,0,2)!=0 && wn==1);                     /* stale or foreign lease */
    CHECK(xhci_contain_w2(0,0x40,0,2)!=0 && wn==1);
    CHECK(xhci_contain_w2(token,0x40,0,3)!=0 && wn==1);                       /* value must be old | HCRST exactly */
    CHECK(xhci_contain_w2(token,0x40,0,0)!=0 && wn==1);
    CHECK(xhci_contain_w2(token,0x40,1,3)!=0 && wn==1);                       /* old is not USBCMD */
    CHECK(xhci_contain_w2(token,0x44,0,2)!=0 && wn==1);                       /* offset is not CAPLENGTH */
    CHECK(xhci_contain_w2(token,0x42,0,2)!=0 && wn==1);                       /* unaligned */
    CHECK(xhci_contain_w2(token,0x4000,0,2)!=0 && wn==1);                     /* outside the lease */
    CHECK(xhci_contain_w2(token,0xfffffffc,0,2)!=0 && wn==1);                 /* overflow */
    exe.armed = 0; CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==1); exe.armed = 1;
    flags = RFLAGS_IF; CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==1); flags = 0;
    fn[0].w[1] |= 4; CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==1); fn[0].w[1] &= ~4u;   /* bus mastering is back on */
    bar[0] = 0x01000041;                                                      /* CAPLENGTH register disagrees with offset */
    CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==1);
    bar[0] = 0x01000040;
    CHECK(xhci_contain_w2(token,0x40,0,2)==0 && wn==2 && wlog[1].kind=='M' && wlog[1].val==2);
    CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==2);                       /* once */
}
static void a_w2_needs_bme_off_now(void)
{
    adapter_setup();
    CHECK(A1(7,3)==0);
    fn[0].w[1] = 0x00100007;                                                 /* a third party re-enabled it after W1 */
    CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==1);
}
static void a_reset_on_stale_usbcmd(void)
{
    adapter_setup(); CHECK(A1(7,3)==0);
    bar[0x40/4] = 1;                                                         /* USBCMD changed since the caller read it */
    CHECK(xhci_contain_w2(token,0x40,0,2)!=0 && wn==1);
}

int main(void)
{
    scenario("C3b.2 eligible QEMU profile: exactly W1 then W2, read-backs, reset, once per boot, firmware memory untouched", x_eligible);
    scenario("C3b.2 disabled by default even in the test image", x_disabled_by_default);
    static const struct { const char *name; void (*body)(void); } cases[] = {
        {"conflicting option vos.usb2-contain=1", x_c_contain}, {"conflicting option vos.usb2-residue=1", x_c_residue},
        {"conflicting option vos.usb2-qualify=1", x_c_qualify}, {"conflicting option vos.usb2c2-dma=1", x_c_dma},
        {"conflicting option vos.usb_probe=1", x_c_probe},
        {"BIOS", x_bios}, {"command 0x107", x_cmd}, {"running controller", x_running}, {"USBSTS.EINT", x_eint},
        {"USBCMD.INTE", x_inte}, {"IMAN.IP", x_ip}, {"MSI-X enabled", x_msix}, {"active slot", x_slot},
        {"port change", x_port}, {"port connected", x_connected}, {"PCIe transactions pending", x_pcie_busy},
        {"profile mismatch", x_profile}, {"NEC controller", x_nec}, {"no controller", x_none}, {"two controllers", x_two},
        {"bad context", x_context}, {"xHCI subsystem not QEMU's", x_subsystem}, {"xHCI subsystem zero", x_subsystem0},
        {"no hypervisor bit", x_no_hv}, {"KVM signature", x_kvm_sig}, {"TCG signature off by one bit", x_sig_bit},
        {"ACPI OEM ID not BOCHS", x_oem}, {"no ACPI tag", x_no_acpi}, {"host bridge not Q35", x_hostbridge},
        {"host bridge subsystem", x_hostbr_sub}, {"no host bridge", x_no_hostbr},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char t[112]; snprintf(t, sizeof t, "C3b.2 refused, nothing written: %s", cases[i].name);
        scenario(t, cases[i].body);
    }
    scenario("C3b.2 W1 ignored by the device: final, no W2, nothing restored", x_w1_ignored);
    scenario("C3b.2 W1 changes the status: final, no W2", x_w1_status);
    scenario("C3b.2 controller error (HCE) after W1: final, no W2, loud integrity failure", x_w1_hce);
    scenario("C3b.2 W1 changes more than Bus Master Enable: final, no W2", x_w1_more);
    scenario("C3b.2 no write ever sets Bus Master Enable", x_bme_never_set);
    scenario("C3b.2 HCRST never clears: final, no abort or retry", x_w2_stuck);
    scenario("C3b.2 CNR never clears: final", x_w2_cnr);
    scenario("C3b.2 event ring pointer survives the reset: unverified", x_w2_erdp);
    scenario("C3b.2 command ring pointer survives the reset: unverified", x_w2_crcr);
    scenario("C3b.2 a port reports a device after the reset: unverified", x_w2_port);
    scenario("C3b.2 IMOD at the specification default is accepted", x_w2_imod);
    scenario("C3b.2 stuck reset polls exactly the bound", x_stuck_polls_bounded);
    scenario("C3b.2 a device appears at every read of a full execution: only valid write prefixes", x_sweep);
    scenario("C3b.2 a device on each port read: the guarding stage is pinned (before W1, after W1, before W2, after reset)", x_ports_guarded);
    scenario("C3b.2 W1 adapter refuses unless all of its own preconditions hold", a_w1);
    scenario("C3b.2 W2 adapter: order, lease, value, offset, bus-master and once-only checks", a_w2_order);
    scenario("C3b.2 W2 adapter refuses if bus mastering was re-enabled after W1", a_w2_needs_bme_off_now);
    scenario("C3b.2 W2 adapter refuses if USBCMD changed since it was read", a_reset_on_stale_usbcmd);
    printf("PASS: real PMM/devmap/qualification/residue/executor C with the bare-metal Rust archive; only W1 and W2 write\n");
    return 0;
}

/* USB.2-C1 hosted tests: the real PMM, devmap, USB resource and qualification
 * C code linked with the actual bare-metal Rust archive. Only CPU state, the
 * physical alias, PCI configuration and BAR registers are modeled. The model
 * has no write path: any PCI write aborts. C sanitizers instrument this C,
 * not the Rust machine code. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define VOS_X86_64_CPU_H
#define VOS_X86_64_MEMLAYOUT_H
#define PAGE_SIZE 4096ULL
#define BOOT_MAP_LIMIT 0x40000000ULL
#define KERNEL_VMA 0xffffffff80000000ULL
#define RFLAGS_IF (1ULL << 9)
static unsigned char ram[2*1024*1024] __attribute__((aligned(4096)));
static uint64_t cr3, flags, pat = 0x0007040600070406ULL, invalidations;
static int in_irq;
static void *phys_to_virt(uint64_t pa) { assert(pa < sizeof(ram)); return ram + pa; }
static uint64_t read_cr3(void) { return cr3; }
static uint64_t read_rflags(void) { return flags; }
static void invlpg(uint64_t va) { (void)va; invalidations++; }
static uint64_t rdmsr(unsigned msr) { (void)msr; return pat; }
static void wrmsr(unsigned msr,uint64_t v) { (void)msr; pat=v; }
static void cpuid(unsigned a,unsigned c,uint32_t *x,uint32_t *b,uint32_t *y,uint32_t *d)
{ (void)c; *x=a==0x80000000?0x80000008:a==0x80000008?40:0; *b=*y=0; *d=1u<<16; }
static uint32_t mmio_model(uint64_t va);
#define XHCI_QUALIFY_READ32(va, pa) mmio_model(va)
#include "../../arch/x86_64/pmm.c"
#include "../../arch/x86_64/devmap.c"
#include "../../arch/x86_64/usb_resources.c"
#include "../../arch/x86_64/xhci_qualify.c"

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

/* ---- PCI and BAR models ---- */
#define FUNCTIONS 3
static struct { int present; struct pci_addr a; uint32_t w[64]; } fn[FUNCTIONS];
static uint32_t bar[4096];          /* 16 KiB register image of function 0 */
static unsigned cfg_reads, mmio_reads, change_after, change_word, legacy_change_after;
static uint32_t change_value;
static unsigned corrupt_pte_after;
static uint64_t corrupt_va, saved_pte;
uint32_t pci_read32(struct pci_addr a, unsigned off)
{
    assert(off < 256 && !(off & 3));
    cfg_reads++;
    for (unsigned i=0;i<FUNCTIONS;i++)
        if (fn[i].present && !memcmp(&fn[i].a,&a,sizeof a)) return fn[i].w[off/4];
    return 0xffffffffu;
}
uint16_t pci_read16(struct pci_addr a, unsigned off) { return pci_read32(a,off&~3u)>>((off&2)*8); }
uint8_t pci_read8(struct pci_addr a, unsigned off) { return pci_read32(a,off&~3u)>>((off&3)*8); }
void pci_write8(struct pci_addr a, unsigned off, uint8_t v) { (void)a;(void)off;(void)v; abort(); }
void pci_write16(struct pci_addr a, unsigned off, uint16_t v) { (void)a;(void)off;(void)v; abort(); }
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
    assert(!devmap_scope_extent(devmap_scope_token(),&sva,&len));
    assert(va>=sva && va<sva+len && !(va&3) && len==16384);
    mmio_reads++;
    if (change_after && mmio_reads==change_after) fn[0].w[change_word]=change_value;
    if (legacy_change_after && mmio_reads==legacy_change_after) bar[0x20/4]^=1u<<24;
    if (corrupt_pte_after && mmio_reads==corrupt_pte_after) {
        corrupt_va=sva; saved_pte=*pte_slot(sva,0); *pte_slot(sva,0)|=PTE_U;
    }
    return bar[(va-sva)/4];
}
static void qemu_function(unsigned i, uint8_t dev)
{
    fn[i].present=1; fn[i].a=(struct pci_addr){0,dev,0};
    memset(fn[i].w,0,sizeof fn[i].w);
    uint32_t *w=fn[i].w;
    w[0]=0x000d1b36; w[1]=0x00100107; w[2]=0x0c033001; w[4]=0xfebf0004;
    w[0x34/4]=0x90; w[0x90/4]=0x000fa011; w[0x94/4]=0x3000; w[0x98/4]=0x3800; w[0xa0/4]=0x00020010;
}
static void qemu_bar(int legacy)
{
    memset(bar,0,sizeof bar);
    bar[0]=0x01000040; bar[1]=0x08001040; bar[2]=0xf; bar[4]=0x00087001; bar[5]=0x2000; bar[6]=0x1000;
    bar[0x48/4]=1; bar[0x44/4]=1; /* PAGESIZE; USBSTS.HCH */
    if (legacy) { bar[0x20/4]=0x00010401; bar[0x24/4]=0xe0000000; }  /* BIOS-owned, next 0x30 */
    else { bar[0x20/4]=0x02000402; bar[0x24/4]=0x20425355; bar[0x28/4]=0x0401; }
    bar[0x30/4]=0x03000002; bar[0x34/4]=0x20425355; bar[0x38/4]=legacy?0x0801:0x0405;
}
static void reset_models(void)
{
    memset(fn,0,sizeof fn); qemu_function(0,2); qemu_bar(0);
    cfg_reads=mmio_reads=change_after=legacy_change_after=0; corrupt_pte_after=0;
    outlen=0; out[0]=0;
}

static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n%s\n",__FILE__,__LINE__,#x,out);abort();} } while(0)
static uint64_t free_frames;
static unsigned maps;
static int restored(void)
{
    return !qual.token && !qual.active && !devmap_scope_token() && pmm_free_count()==free_frames &&
           devmap_count()==maps && !devmap_audit();
}
static int run(const char *cmdline) { outlen=0; out[0]=0; return xhci_qualify_boot(cmdline); }

int main(void)
{
    struct { struct mb2_tag_mmap header; struct mb2_mmap_entry entries[2]; } mm = {
        .header={.entry_size=sizeof(struct mb2_mmap_entry)},
        .entries={{.base=0x100000,.length=0x100000,.type=MB2_MMAP_AVAILABLE},
                  {.base=0x100000000ULL,.length=0x4000,.type=2}}
    };
    struct boot_info bi={.mmap=&mm.header,.mmap_entries=2};
    CHECK(pmm_init(&bi,NULL)==0);
    uint64_t pdpt;
    CHECK(pmm_alloc_frame(&root)==0); CHECK(pmm_alloc_frame(&pdpt)==0);
    ((uint64_t *)phys_to_virt(root))[511]=pdpt|PTE_P|PTE_W;
    cr3=root; CHECK(devmap_init(&bi)==0);
    void *framebuffer;
    CHECK(!devmap_map(0xfd000000ULL,0x300000,DEVMAP_WC,"framebuffer",&framebuffer,NULL));
    uint64_t fb_pte=devmap_expected_pte((uint64_t)(uintptr_t)framebuffer);
    free_frames=pmm_free_count(); maps=devmap_count();

    /* Not requested: no reads, output or effects; exact token matching. */
    reset_models();
    CHECK(run("") == 1 && !outlen && !cfg_reads && restored());
    CHECK(run("vos.usb2-qualify=10 vos.usb2-qualify") == 1 && !outlen && !cfg_reads);
    CHECK(xhci_qualify_conflict("vos.usb_probe=1 vos.usb2-qualify=1"));
    CHECK(xhci_qualify_conflict("vos.usb2-qualify=1\tvos.usb_probe=1"));
    CHECK(!xhci_qualify_conflict("vos.usb2-qualify=1") && !xhci_qualify_conflict("vos.usb_probe=1"));
    CHECK(!xhci_qualify_conflict("vos.usb_probe=11 vos.usb2-qualify=1"));

    /* Supported profile: qualified, 18 reads, everything restored. */
    reset_models();
    uint64_t generation=scope.generation;
    CHECK(run("vos.usb2-qualify=1") == 1);
    CHECK(strstr(out,"usb2c1: QUALIFIED: profile qualified") && strstr(out,"mmio_reads=0x12"));
    CHECK(strstr(out,"command_before=0x107") && strstr(out,"command_after=0x107"));
    CHECK(strstr(out,"lease_acquired=0x1") && strstr(out,"lease_released=0x1") && strstr(out,"resources restored"));
    CHECK(mmio_reads==18 && restored() && scope.generation==generation+1);
    CHECK(devmap_expected_pte((uint64_t)(uintptr_t)framebuffer)==fb_pte);

    /* BIOS-owned legacy support is observed only; values are reported raw. */
    reset_models(); qemu_bar(1);
    CHECK(run("vos.usb2-qualify=1") == 1 && strstr(out,"QUALIFIED"));
    CHECK(strstr(out,"legacy_offset=0x20") && strstr(out,"usblegsup_before=0x10401") &&
          strstr(out,"usblegsup_after=0x10401") && strstr(out,"usblegctlsts_before=0xe0000000"));
    CHECK(bar[0x20/4]==0x00010401 && restored());

    /* Fail-closed rejections: nothing mapped before the rejecting check. */
    reset_models(); fn[0].present=0;
    generation=scope.generation;
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: no xHCI on bus 0") && scope.generation==generation && restored());
    reset_models(); qemu_function(1,3);
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: ambiguous xHCI candidates") && scope.generation==generation && restored());
    reset_models(); fn[0].w[0]=0x01941033;
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: unsupported identity") && scope.generation==generation && restored());
    reset_models(); fn[0].w[4]=0xfebf1004;
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: BAR misaligned") && !mmio_reads && restored());
    reset_models(); fn[0].w[0xa0/4]=0x00029010; fn[0].w[0x90/4]=0x000fa011;
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: capability cycle") && restored());
    reset_models(); bar[0x20/4]=0x00000401; bar[0x30/4]=0x00000001; /* duplicate legacy */
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: malformed capabilities") &&
          strstr(out,"lease_released=0x1") && restored());
    reset_models(); bar[2]=0x0800000f; /* scratchpads */
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: unsupported register profile") && restored());

    /* BAR inside available RAM: lease refused. */
    reset_models(); fn[0].w[4]=0x00104004; /* BAR inside available RAM */
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: BAR lease refused") && restored());

    /* Partial page-table failure: exact rollback, nothing retained. */
    reset_models(); devmap_scope_budget(0);
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"BAR lease refused") && strstr(out,"no frame for a page table") && restored());
    devmap_scope_budget(-1);

    /* Wrong CR3, IF set, IRQ context: the lease and reads are refused. */
    for (int c=0;c<3;c++) {
        reset_models(); cr3=c==0?root+4096:root; flags=c==1?RFLAGS_IF:0; in_irq=c==2;
        CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"BAR lease refused") && !mmio_reads);
        cr3=root; flags=0; in_irq=0; CHECK(restored());
    }

    /* Direct session API: stale, double, cross-kind, base mismatch, bounds. */
    reset_models(); qual.fn=fn[0].a; qual.active=1;
    struct usb_resource r, f;
    uint32_t v;
    CHECK(xhci_qualify_bar_acquire(0xfebf4000ULL,&r)==DEVMAP_ESTATE && !r.token);
    CHECK(xhci_qualify_bar_acquire(0xfebf0000ULL,&r)==0 && r.token && devmap_scope_tables()==1);
    CHECK(pmm_free_count()==free_frames-1 && devmap_scope_kind()==DEVMAP_SCOPE_XHCI_BAR);
    CHECK(xhci_qualify_bar_acquire(0xfebf0000ULL,&f)==DEVMAP_ESTATE);
    CHECK(usb_resource_acquire(USB_RESOURCE_MAPPING,&f)==DEVMAP_ESTATE);
    void *late=NULL; CHECK(devmap_map(0xf1000000ULL,4096,DEVMAP_UC,"late",&late,NULL)==DEVMAP_ESTATE);
    CHECK(!*pte_slot(r.address+16384,0)); /* guard page */
    for (unsigned n=0;n<4;n++)CHECK((*pte_slot(r.address+n*4096,0)&~(PTE_A|PTE_D))==
        ((0xfebf0000ULL+n*4096)|PTE_P|PTE_W|PTE_NX|PTE_PCD|PTE_PWT));
    CHECK(xhci_qualify_bar_read32(r.token,0,&v)==0 && v==0x01000040);
    CHECK(xhci_qualify_bar_read32(r.token,16384,&v)==DEVMAP_EINVAL && !v);
    CHECK(xhci_qualify_bar_read32(r.token,2,&v)==DEVMAP_EINVAL);
    CHECK(xhci_qualify_bar_read32(r.token+1,0,&v)==DEVMAP_ESTATE);
    CHECK(usb_resource_release(USB_RESOURCE_MAPPING,r.token)==DEVMAP_ESTATE && devmap_scope_token()==r.token);
    CHECK(xhci_qualify_bar_release(r.token+1)==DEVMAP_ESTATE);
    cr3=root+4096; CHECK(xhci_qualify_bar_release(r.token)==DEVMAP_ESTATE); cr3=root;
    CHECK(xhci_qualify_bar_release(r.token)==0 && xhci_qualify_bar_release(r.token)==DEVMAP_ESTATE);
    CHECK(xhci_qualify_bar_read32(r.token,0,&v)==DEVMAP_ESTATE);
    qual.active=0; CHECK(restored());
    CHECK(xhci_qualify_bar_acquire(0xfebf0000ULL,&r)==DEVMAP_ESTATE); /* outside a session */
    uint32_t cv; CHECK(xhci_qualify_config32(0,&cv)==DEVMAP_EINVAL);
    qual.active=1; CHECK(xhci_qualify_config32(3,&cv)==DEVMAP_EINVAL && xhci_qualify_config32(256,&cv)==DEVMAP_EINVAL);
    qual.active=0;
    /* The fixture lease cannot be released through the BAR interface. */
    CHECK(usb_resource_acquire(USB_RESOURCE_MAPPING,&f)==0);
    qual.token=f.token; CHECK(xhci_qualify_bar_release(f.token)==DEVMAP_ESTATE); qual.token=0;
    CHECK(!usb_resource_release(USB_RESOURCE_MAPPING,f.token) && restored());

    /* Integrity: PCI command changed mid-qualification (other agent). */
    reset_models(); change_after=5; change_word=1; change_value=0x00100103;
    CHECK(run("vos.usb2-qualify=1")==0 && strstr(out,"FAIL: state changed") &&
          strstr(out,"command_after=0x103") && strstr(out,"integrity failure; resources restored") && restored());
    reset_models(); change_after=5; change_word=0x90/4; change_value=0x800fa011; /* MSI-X enabled */
    CHECK(run("vos.usb2-qualify=1")==0 && strstr(out,"FAIL: state changed") && restored());
    reset_models(); qemu_bar(1); legacy_change_after=16; /* OS-owned flips between observations */
    CHECK(run("vos.usb2-qualify=1")==0 && strstr(out,"FAIL: state changed") && restored());

    /* Release failure retains the lease; explicit owner recovery afterwards. */
    reset_models(); corrupt_pte_after=3;
    CHECK(run("vos.usb2-qualify=1")==0 && strstr(out,"FAIL: lease retained") && strstr(out,"lease_released=0x0") &&
          strstr(out,"integrity failure; lease retained"));
    CHECK(qual.token && devmap_scope_token()==qual.token && qual.retained && pmm_free_count()==free_frames-1);
    *pte_slot(corrupt_va,0)=saved_pte;
    CHECK(xhci_qualify_bar_release(qual.token)==0 && !qual.token && restored());

    /* Rust ABI interface checks. */
    CHECK(vos_xhci_qualify_v1(1,1,40,NULL)==-2);
    _Alignas(8) unsigned char raw[sizeof(struct xhci_qualify_report)+8];
    CHECK(vos_xhci_qualify_v1(1,1,40,(void *)(raw+4))==-2);
    reset_models(); qual.fn=fn[0].a; qual.active=1;
    struct xhci_qualify_report rep; memset(&rep,0xa5,sizeof rep);
    CHECK(vos_xhci_qualify_v1(0,1,40,&rep)==1 && rep.reason==1 && rep.candidates==0 && !rep.base && !rep.mmio_reads);
    CHECK(vos_xhci_qualify_v1(1,1,31,&rep)==1 && rep.reason==4);
    qual.active=0; CHECK(restored() && pmm_check_in(pmm_kernel())==0 && invalidations>0);

    /* Last: a USB.1-style permanent mapping of the BAR (never released, so it
     * stays). Qualification must refuse the overlapping lease. */
    reset_models();
    void *usb1=NULL;
    CHECK(!devmap_map(0xfebf0000ULL,28,DEVMAP_UC,"USB.1 xHCI capabilities",&usb1,NULL));
    unsigned with_usb1=devmap_count(); uint64_t free_usb1=pmm_free_count();
    CHECK(run("vos.usb2-qualify=1")==1 && strstr(out,"REJECTED: BAR lease refused") &&
          strstr(out,"overlaps another device mapping") && !mmio_reads);
    CHECK(!devmap_scope_token() && devmap_count()==with_usb1 && pmm_free_count()==free_usb1 && !devmap_audit());

    printf("PASS: %u assertions; actual PMM/devmap/USB resource/xhci_qualify C with the bare-metal Rust "
           "archive: profile, rejections, overlap/RAM, PT failure, CR3/IF/IRQ, stale/double/cross-kind "
           "tokens, bounds, state-change integrity, retention and recovery; zero PCI writes\n",checks);
#ifdef USB2C1_BENCH
    /* One-time qualification cost of the C+Rust logic with modeled MMIO and
     * real PMM/devmap work. Optimized, unsanitized; not a hardware latency. */
    for (unsigned sample=0;sample<7;sample++) {
        struct timespec a,b;
        clock_gettime(CLOCK_MONOTONIC,&a);
        for (unsigned n=0;n<1000;n++) { reset_models(); CHECK(run("vos.usb2-qualify=1")==1); }
        clock_gettime(CLOCK_MONOTONIC,&b);
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000ULL+b.tv_nsec-a.tv_nsec;
        printf("sample=%u ns_per_qualification=%llu\n",sample,(unsigned long long)(ns/1000));
    }
#endif
    return 0;
}

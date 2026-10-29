/* USB.2-C3a hosted tests: the real PMM (with pmm_reserve), devmap, USB
 * resource and qualification/residue C code linked with the actual Rust
 * archive. An OVMF-like firmware residue lives in modeled RAM inside the
 * allocator's range. Each guard scenario runs in a forked child (the guard is
 * once per boot and its reservations are permanent by design). The model has
 * no write path: a PCI write aborts. C sanitizers do not instrument Rust. */
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
static void cpuid(unsigned a,unsigned c,uint32_t *x,uint32_t *b,uint32_t *y,uint32_t *d)
{ (void)c; *x=a==0x80000000?0x80000008:a==0x80000008?40:0; *b=*y=0; *d=1u<<16; }
static uint32_t mmio_model(uint64_t va);
#define XHCI_QUALIFY_READ32(va, pa) mmio_model(va)
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
#define FUNCTIONS 2
static struct { int present; struct pci_addr a; uint32_t w[64]; } fn[FUNCTIONS];
static uint32_t bar[4096];
static unsigned change_after, mmio_reads;
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
    assert(!devmap_scope_extent(devmap_scope_token(),&sva,&len) && va>=sva && va<sva+len && !(va&3));
    if (++mmio_reads==change_after) fn[0].w[1]=0x00100103;   /* another agent clears BME */
    return bar[(va-sva)/4];
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
    change_after=0; mmio_reads=0; outlen=0; out[0]=0;
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
static int guard(void) { outlen=0; out[0]=0; return xhci_residue_boot("vos.usb2-residue=1"); }
static int state(uint64_t f) { return pmm_frame_state_in(pmm_kernel(),f); }

static void scenario(const char *name, void (*body)(void))
{
    fflush(stdout);
    pid_t pid=fork();
    if (!pid) { setup(); body(); printf("  %s: %u checks PASS\n",name,checks); fflush(stdout); _exit(0); }
    int status=0; assert(waitpid(pid,&status,0)==pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr,"scenario %s FAILED\n",name); exit(1); }
}

static void complete(void)
{
    CHECK(xhci_residue_boot("")==1 && !outlen && !residue.count);
    CHECK(guard()==1 && strstr(out,"usb2c3a: PLAN VALID (dry run, not executed)"));
    CHECK(strstr(out,"usb2c1: bus=0") && strstr(out,"plan=0x10 0x2 0x3 0x4 0x5 0x6"));
    CHECK(strstr(out,"findings=0x0") && strstr(out,"reserved=0x4") && strstr(out,"hazards=0x0"));
    CHECK(residue.count==4 && pmm_free_count()==base-4);
    for (uint64_t f=0x1a0000; f<0x1a4000; f+=4096) CHECK(state(f)==PMM_FRAME_ALLOCATED && residue_owned(f));
    CHECK(!devmap_scope_token() && !qual.token && !devmap_audit() && pmm_check_in(pmm_kernel())==PMM_OK);
    CHECK(strstr(out,"lease_released=0x1") && strstr(out,"command_after=0x107"));
    /* Once per boot; reservations survive and no release path exists. */
    CHECK(guard()==0 && residue.count==4 && pmm_free_count()==base-4);
    CHECK(xhci_residue_reserve(0x1b0000,1)==-4);             /* outside a session */
    uint64_t v; CHECK(xhci_residue_read64(DCBAA,&v)==-1);    /* outside a session */
}
static void hazard(void)
{
    CHECK(pmm_reserve_in(&kernel_pmm,0x1a2000)==PMM_OK);     /* the kernel already holds this frame */
    base=pmm_free_count();
    CHECK(guard()==1 && strstr(out,"REFUSED: HAZARD") && strstr(out,"hazards=0x1"));
    CHECK(!residue_owned(0x1a2000) && residue.count==3 && pmm_free_count()==base-3);
    CHECK(strstr(out,"refusal=0x2"));                         /* residue incomplete */
}
static void protected_map(void)
{
    mm.entries[2].length=0x8000;                              /* residue in a reserved entry */
    kernel_pmm.state=PMM_UNINIT; CHECK(pmm_init(&bi,NULL)==0);
    CHECK(pmm_alloc_frame(&root)==0); uint64_t pdpt; CHECK(pmm_alloc_frame(&pdpt)==0);
    ((uint64_t *)phys_to_virt(root))[511]=pdpt|PTE_P|PTE_W; cr3=root;
    memset(&scope,0,sizeof scope); count=0; ready=0; CHECK(devmap_init(&bi)==0);
    model(); base=pmm_free_count();
    CHECK(guard()==1 && strstr(out,"REFUSED: residue incomplete") && strstr(out,"reserved=0x0"));
    CHECK(strstr(out,"findings=0x2") && !residue.count && pmm_free_count()==base);
}
static void exhausted(void)
{
    put64(ERST+8,4096); put64(ERST,0x1c0000); bar[0x1038/4]=0x1c0000;   /* 16-page segment */
    CHECK(guard()==1 && strstr(out,"REFUSED") && residue.count==XHCI_RESIDUE_FRAMES);
    CHECK(pmm_free_count()==base-XHCI_RESIDUE_FRAMES && xhci_residue_reserve(0x1f0000,4)==-4);
    residue.session=1; CHECK(xhci_residue_reserve(0x1f0000,4)==-2); residue.session=0;  /* full: fail closed */
}
static void running(void)
{
    bar[0x40/4]=1; bar[0x44/4]=0;                             /* R/S set, not halted */
    CHECK(guard()==1 && strstr(out,"REFUSED") && strstr(out,"findings=0x1") && residue.count==4);
}
static void devices(void)
{
    put64(DCBAA+8*2,0x1c0000);                                /* slot 2 device context */
    CHECK(guard()==1 && strstr(out,"REFUSED") && strstr(out,"findings=0x20"));
    CHECK(residue_owned(0x1c0000) && residue.count==5);
}
static void unqualified(void)
{
    fn[0].w[0]=0x01941033;                                    /* NEC identity */
    CHECK(guard()==1 && strstr(out,"REJECTED") == NULL && strstr(out,"REFUSED") && !residue.count);
    CHECK(strstr(out,"refusal=0x1") && pmm_free_count()==base && !mmio_reads);
}
static void no_controller(void)
{
    fn[0].present=0;
    CHECK(guard()==1 && strstr(out,"REFUSED") && !residue.count && pmm_free_count()==base && !mmio_reads);
}
static void state_change(void)
{
    change_after=20;                                          /* PCI command changes mid-guard */
    CHECK(guard()==0 && strstr(out,"FAIL: integrity"));
    CHECK(residue.count==4 && pmm_free_count()==base-4);      /* reservations kept, never undone */
}
static void bad_context(void)
{
    flags=RFLAGS_IF;
    CHECK(guard()==1 && strstr(out,"REFUSED") && !residue.count && pmm_free_count()==base);
}
static void direct_api(void)
{
    uint64_t v;
    CHECK(pmm_reserve_in(&kernel_pmm,0x1a0001)==PMM_EALIGN && pmm_reserve_in(&kernel_pmm,0x40000000)==PMM_ERANGE);
    CHECK(pmm_reserve_in(&kernel_pmm,0x80000)==PMM_EPROTECTED);
    CHECK(pmm_reserve_in(&kernel_pmm,root)==PMM_EINUSE && pmm_free_count()==base);
    residue.session=1; qual.active=1;
    CHECK(xhci_residue_reserve(root,1)==-4 && !residue.count);           /* allocated: never claimed */
    CHECK(xhci_residue_reserve(0x1b0000,1)==0 && xhci_residue_reserve(0x1b0000,1)==-3);
    CHECK(residue.count==1 && pmm_free_count()==base-1);                 /* duplicate: no double count */
    CHECK(xhci_residue_read64(0x1b0008,&v)==0 && xhci_residue_read64(0x1b0004,&v)==-1);
    CHECK(xhci_residue_read64(0x1c0000,&v)==-1 && !v);                   /* not owned */
    CHECK(xhci_residue_page(0x1b0000)==1 && xhci_residue_page(root)==2 && xhci_residue_page(0x80000)==3);
    CHECK(xhci_residue_page(0x40000000)==4 && xhci_residue_page(0x1d0000)==0 && xhci_residue_page(0x1d0001)==3);
    residue.session=0; qual.active=0;
    CHECK(xhci_qualify_conflict("vos.usb_probe=1 vos.usb2-residue=1") && !xhci_qualify_conflict("vos.usb2-residue=1"));
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
static uint8_t snap[0x8000];
static void excluded_inspected(void)
{
    efi_boot(8,1);
    for (uint64_t f=0x1a0000; f<0x1a8000; f+=4096) CHECK(state(f)==PMM_FRAME_UNAVAILABLE);
    memcpy(snap,ram+0x1a0000,sizeof snap);
    xhci_residue_boot_memory(&ebi,&et);
    CHECK(guard()==1 && strstr(out,"usb2c3a: PLAN VALID (dry run, not executed)"));
    CHECK(strstr(out,"findings=0x0") && strstr(out,"reserved=0x0") && strstr(out,"excluded=0x7"));
    CHECK(strstr(out,"inspection=0x3") && strstr(out,"reuse_protected=1 inspection_complete=1; firmware memory is never released"));
    CHECK(!residue.count && pmm_free_count()==base && !memcmp(snap,ram+0x1a0000,sizeof snap));
    for (uint64_t f=0x1a0000; f<0x1a8000; f+=4096) CHECK(state(f)==PMM_FRAME_UNAVAILABLE);
    CHECK(!residue.bi && !residue.resv);                     /* session inputs dropped */
    uint64_t v; CHECK(xhci_residue_read64(DCBAA,&v)==-1 && !v); /* outside a session */
    CHECK(!devmap_scope_token() && !qual.token && !devmap_audit() && pmm_check_in(pmm_kernel())==PMM_OK);
}
static void excluded_without_inputs(void)
{
    efi_boot(8,1);                                           /* boot memory never handed to the guard */
    CHECK(guard()==1 && strstr(out,"REFUSED: residue incomplete") && strstr(out,"findings=0x2"));
    CHECK(strstr(out,"excluded=0x0") && strstr(out,"inspection=0x1") && !residue.count && pmm_free_count()==base);
}
static void protected_not_excluded(void)
{
    em.entries[2].base=0x1a0000; em.entries[2].length=0x8000; /* map-reserved, not an EFI exclusion */
    efi_boot(8,0);
    xhci_residue_boot_memory(&ebi,&et);
    CHECK(xhci_residue_page(0x1a0000)==3 && xhci_residue_page(0x1a3000)==3);
    CHECK(guard()==1 && strstr(out,"REFUSED: residue incomplete") && strstr(out,"excluded=0x0"));
    CHECK(strstr(out,"findings=0x2") && !residue.count && pmm_free_count()==base);
}
static void excluded_touching_reserved(void)
{
    em.entries[2].base=0x1a0800; em.entries[2].length=0x100;   /* reserved bytes inside the DCBAA page */
    efi_boot(8,1);
    xhci_residue_boot_memory(&ebi,&et);
    CHECK(xhci_residue_page(0x1a0000)==3 && xhci_residue_page(0x1a1000)==5);
    memcpy(snap,ram+0x1a0000,sizeof snap);
    CHECK(guard()==1 && strstr(out,"REFUSED: residue incomplete") && strstr(out,"reserved=0x0"));
    CHECK(!residue.count && pmm_free_count()==base && !memcmp(snap,ram+0x1a0000,sizeof snap));
}
static void corrupted_map(void)
{
    efi_boot(8,1);
    efi17[12]=2;                                             /* EFI map no longer valid at guard time */
    xhci_residue_boot_memory(&ebi,&et);
    CHECK(xhci_residue_page(0x1a0000)==3);
    CHECK(guard()==1 && strstr(out,"REFUSED: residue incomplete") && strstr(out,"excluded=0x0"));
}
static void excluded_and_free(void)
{
    efi_boot(3,1);                                           /* 0x1a3000 is ordinary free RAM */
    xhci_residue_boot_memory(&ebi,&et);
    CHECK(guard()==1 && strstr(out,"usb2c3a: PLAN VALID") && strstr(out,"reserved=0x1") && strstr(out,"owned_again=0x1") && strstr(out,"excluded=0x5"));
    CHECK(residue.count==1 && residue.frame[0]==0x1a3000 && pmm_free_count()==base-1);
    CHECK(state(0x1a3000)==PMM_FRAME_ALLOCATED && state(0x1a2000)==PMM_FRAME_UNAVAILABLE);
}
static void excluded_direct(void)
{
    uint64_t v;
    efi_boot(8,1);
    xhci_residue_boot_memory(&ebi,&et);
    CHECK(xhci_residue_page(0x1a2000)==5 && xhci_residue_page(0x1b0000)==0 && xhci_residue_page(0x1a2001)==3);
    residue.session=1;
    CHECK(xhci_residue_read64(SEG+8,&v)==0 && xhci_residue_read64(SEG+4,&v)==-1 && xhci_residue_read64(0x1b0000,&v)==-1);
    CHECK(xhci_residue_reserve(0x1a2000,4)==-4 && !residue.count);   /* excluded: never reservable */
    residue.session=0;
    CHECK(xhci_residue_read64(SEG+8,&v)==-1);
    ebi.efi=0; ebi.efi_mmap_tags=0;                          /* BIOS view: no EFI exclusion exists */
    CHECK(xhci_residue_page(0x1a2000)==3);
    ebi.efi=1; ebi.efi_mmap_tags=2;                          /* contradictory loader evidence */
    CHECK(xhci_residue_page(0x1a2000)==3);
    ebi.efi_mmap_tags=1; xhci_residue_boot_memory(NULL,NULL);
    CHECK(xhci_residue_page(0x1a2000)==3 && efi_memory_firmware_frame(&ebi,NULL,0x1a2000)==0);
    CHECK(efi_memory_firmware_frame(&ebi,&et,0x1a2000)==1 && vos_efi_firmware_frame_v1(NULL,0x1a2000)==-1);
    CHECK(pmm_free_count()==base);
}

int main(void)
{
    scenario("OVMF-like residue: complete, reserved, plan valid, once per boot", complete);
    scenario("already-allocated frame: hazard, not reserved", hazard);
    scenario("residue in a reserved map entry: uninspected, refused", protected_map);
    scenario("registry exhaustion fails closed", exhausted);
    scenario("running controller refused, structures protected", running);
    scenario("device context present: protected, incomplete", devices);
    scenario("unsupported controller: refused, no MMIO, nothing reserved", unqualified);
    scenario("no controller: refused, nothing reserved", no_controller);
    scenario("state change mid-guard: integrity failure, reservations kept", state_change);
    scenario("bad context: lease refused, nothing reserved", bad_context);
    scenario("direct PMM and registry API", direct_api);
    scenario("C3a.3 excluded firmware RAM: inspected in place, plan valid, nothing reserved or changed", excluded_inspected);
    scenario("C3a.3 excluded RAM without boot inputs: uninspected, refused", excluded_without_inputs);
    scenario("C3a.3 map-reserved (not EFI-excluded) residue: uninspected, refused", protected_not_excluded);
    scenario("C3a.3 excluded page touching reserved bytes: that page uninspected, refused", excluded_touching_reserved);
    scenario("C3a.3 EFI map invalid at guard time: fail closed", corrupted_map);
    scenario("C3a.3 excluded and free frames: free one reserved, excluded read in place", excluded_and_free);
    scenario("C3a.3 direct classification, read gating and loader-evidence refusals", excluded_direct);
    printf("PASS: real PMM/devmap/qualification/residue C with the bare-metal Rust archive; zero PCI writes\n");
    return 0;
}

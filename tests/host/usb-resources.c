/* Real production PMM/devmap/resource code; only CPU, physical alias and
 * scheduler-observation boundaries are hosted. Native workers are tested in
 * VOS, not simulated here. No actual MMIO address is dereferenced. */
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
static int in_irq, worker_state;
static void *phys_to_virt(uint64_t pa) { assert(pa < sizeof(ram)); return ram + pa; }
static uint64_t read_cr3(void) { return cr3; }
static uint64_t read_rflags(void) { return flags; }
static void invlpg(uint64_t va) { (void)va; invalidations++; }
static uint64_t rdmsr(unsigned msr) { (void)msr; return pat; }
static void wrmsr(unsigned msr,uint64_t v) { (void)msr; pat=v; }
static void cpuid(unsigned a,unsigned c,uint32_t *x,uint32_t *b,uint32_t *y,uint32_t *d)
{ (void)c; *x=a==0x80000000?0x80000008:a==0x80000008?36:0; *b=*y=0; *d=1u<<16; }
#include "../../arch/x86_64/pmm.c"
#include "../../arch/x86_64/devmap.c"
#include "../../arch/x86_64/usb_resources.c"

static uint64_t root;
uint64_t vmm_kernel_root(void) { return root; }
int irq_in_handler(void) { return in_irq; }
int thread_state(uint64_t id) { (void)id; return worker_state; }
const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi,uint32_t i)
{ return (const void *)((const uint8_t *)bi->mmap+sizeof(*bi->mmap)+i*bi->mmap->entry_size); }
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr,"unexpected panic: %s\n",s); abort(); }

static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while(0)
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
    void *permanent;
    CHECK(!devmap_map(0xf0000000ULL,4096,DEVMAP_UC,"existing mapping",&permanent,NULL));
    uint64_t permanent_pte=devmap_expected_pte((uint64_t)(uintptr_t)permanent);
    uint64_t free=pmm_free_count(),token=0,va=0;
    CHECK(devmap_scope_acquire(0,0,16384,DEVMAP_UC,&token,&va)==DEVMAP_EINVAL);
    CHECK(devmap_scope_acquire(1,1,4096,0,&token,&va)==DEVMAP_EINVAL);
    CHECK(devmap_scope_acquire(1,0,4095,0,&token,&va)==DEVMAP_EINVAL);
    CHECK(devmap_scope_acquire(1,UINT64_MAX,4096,0,&token,&va)==DEVMAP_EINVAL);
    CHECK(devmap_scope_acquire(1,0,16384,DEVMAP_WC,&token,&va)==DEVMAP_EINVAL);
    CHECK(devmap_scope_acquire(1,16384,4096,0,&token,&va)==DEVMAP_EINVAL);
    for(int c=0;c<3;c++) {
        cr3=c==0?root+4096:root; flags=c==1?RFLAGS_IF:0; in_irq=c==2;
        CHECK(devmap_scope_acquire(1,0,16384,0,&token,&va)==DEVMAP_ESTATE);
        CHECK(pmm_free_count()==free && !devmap_scope_token());
    }
    cr3=root;flags=0;in_irq=0;
    unsigned old_count=count; count=DEVMAP_MAX;
    CHECK(devmap_scope_acquire(1,0,16384,0,&token,&va)==DEVMAP_EFULL); count=old_count;
    maxphys=32;CHECK(devmap_scope_acquire(1,0,16384,0,&token,&va)==DEVMAP_ERANGE);maxphys=36;
    mm.entries[1].type=1;CHECK(devmap_scope_acquire(1,0,16384,0,&token,&va)==DEVMAP_ERAM);
    mm.entries[1].type=2;
    for(int budget=0;budget<2;budget++) {
        devmap_scope_budget(budget);
        CHECK(devmap_scope_acquire(1,0,16384,0,&token,&va)==DEVMAP_ENOMEM);
        CHECK(!token && !va && !devmap_scope_token() && pmm_free_count()==free && !devmap_audit());
    }
    devmap_scope_budget(-1);
    for(unsigned i=0;i<64;i++) {
        CHECK(!devmap_scope_acquire(1,0,16384,0,&token,&va));
        void *denied=NULL;
        CHECK(devmap_map(0xf1000000ULL,4096,DEVMAP_UC,"late mapping",&denied,NULL)==DEVMAP_ESTATE);
        CHECK(pmm_free_count()==free-2 && devmap_scope_tables()==2 && !devmap_audit());
        for(unsigned n=0;n<4;n++)CHECK((*pte_slot(va+n*4096,0)&~(PTE_A|PTE_D))==
            ((0x100000000ULL+n*4096)|PTE_P|PTE_W|PTE_NX|PTE_PCD|PTE_PWT));
        CHECK(!*pte_slot(va+16384,0));
        uint64_t invalid=token+1;CHECK(devmap_scope_release(invalid)==DEVMAP_ESTATE);
        cr3=root+4096;CHECK(devmap_scope_release(token)==DEVMAP_ESTATE);cr3=root;
        uint64_t leaf=*pte_slot(va,0);*pte_slot(va,0)|=PTE_U;
        CHECK(devmap_scope_release(token)==DEVMAP_ESTATE && devmap_scope_token()==token);
        *pte_slot(va,0)=leaf;CHECK(!devmap_scope_release(token));
        CHECK(pmm_free_count()==free && !devmap_audit() && !devmap_scope_token());
        CHECK(devmap_expected_pte((uint64_t)(uintptr_t)permanent)==permanent_pte);
        CHECK(devmap_scope_release(token)==DEVMAP_ESTATE);
    }
    struct usb_resource r,r2;
    page_budget=0;CHECK(usb_resource_acquire(1,&r)==DEVMAP_ENOMEM && !r.token);
    page_budget=-1;CHECK(!usb_resource_acquire(1,&r));
    CHECK(usb_resource_acquire(1,&r2)==DEVMAP_ESTATE);
    usb_resource_retain(1,r.token);CHECK(control.retained && control.page==r.address);
    control.worker=123;
    for(int state=THREAD_RUNNABLE;state<=THREAD_DEAD;state++) {
        worker_state=state;CHECK(usb_resource_release(1,r.token)==DEVMAP_ESTATE);
        CHECK(control.token==r.token && pmm_free_count()==free-1);
    }
    worker_state=THREAD_FREE;cr3=root+4096;
    CHECK(usb_resource_release(1,r.token)==DEVMAP_ESTATE);cr3=root;
    kernel_pmm.state=PMM_FAILED;CHECK(usb_resource_release(1,r.token)==DEVMAP_ESTATE);
    CHECK(control.token==r.token && control.page==r.address);kernel_pmm.state=PMM_READY;
    CHECK(!usb_resource_release(1,r.token));CHECK(usb_resource_release(1,r.token)==DEVMAP_ESTATE);
    for(unsigned stage=0;stage<4;stage++) {
        page_budget=stage==0?0:-1;devmap_scope_budget(stage==1?0:stage==2?1:-1);
        CHECK(vos_rust_resource_cycle()==(stage==0?1:stage<3?2:0));
        CHECK(pmm_free_count()==free && !control.token && !devmap_scope_token());
    }
    CHECK(pmm_check_in(pmm_kernel())==0);CHECK(invalidations>=512);
    CHECK(!vos_rust_resource_abandon());CHECK(control.retained && control.token && control.page);
    CHECK(usb_resource_acquire(1,&r)==DEVMAP_ESTATE);
    CHECK(!usb_resource_release(1,control.token));CHECK(pmm_free_count()==free);
    printf("PASS: %u assertions; actual PMM/devmap/C-Rust ownership, rollback, stale tokens, context and retention\n",checks);
#ifdef USB2B_BENCH
    /* Optimized host timings, separately compiled without sanitizers. These
     * measure real allocator/table algorithms with mocked privileged effects,
     * NOT hardware MMIO, TLB timing or a claim about physical computers. */
    for(unsigned sample=0;sample<7;sample++)for(unsigned path=0;path<4;path++) {
        devmap_scope_budget(-1);
        struct timespec begin,end;
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&begin));
        for(unsigned n=0;n<1000;n++) {
            if(path==0) {
                CHECK(!usb_resource_acquire(1,&r));CHECK(!usb_resource_acquire(2,&r2));
                CHECK(!usb_resource_release(2,r2.token));CHECK(!usb_resource_release(1,r.token));
                CHECK(usb_resource_release(2,r2.token)!=0 && usb_resource_release(1,r.token)!=0);
            } else {
                devmap_scope_budget(path==2?0:path==3?1:-1);
                CHECK(vos_rust_resource_cycle()==(path>=2?2:0));
            }
        }
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&end));
        uint64_t ns=(uint64_t)(end.tv_sec-begin.tv_sec)*1000000000ULL+end.tv_nsec-begin.tv_nsec;
        printf("sample=%u path=%u ns_per_operation=%llu\n",sample,path,(unsigned long long)(ns/1000));
        CHECK(pmm_free_count()==free && !devmap_audit());
    }
#endif
    return 0;
}

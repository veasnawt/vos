/* USB.2-C2 hosted tests: the real PMM and USB DMA adapter C code linked with
 * the actual bare-metal Rust archive (pool + exercise). Only CPU state and the
 * physical alias are modeled; no device exists. Each Rust exercise scenario
 * runs in a forked child because the exercise is once-per-boot by design.
 * C sanitizers instrument this C, not the Rust machine code. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define VOS_X86_64_CPU_H
#define VOS_X86_64_MEMLAYOUT_H
#define PAGE_SIZE 4096ULL
#define BOOT_MAP_LIMIT 0x40000000ULL
#define KERNEL_VMA 0xffffffff80000000ULL
#define RFLAGS_IF (1ULL << 9)
static unsigned char ram[2*1024*1024] __attribute__((aligned(4096)));
static uint64_t cr3, flags;
static int in_irq;
static void *phys_to_virt(uint64_t pa) { assert(pa < sizeof(ram)); return ram + pa; }
static uint64_t read_cr3(void) { return cr3; }
static uint64_t read_rflags(void) { return flags; }
#include "../../arch/x86_64/pmm.c"
#include "../../arch/x86_64/usb_dma.c"

static uint64_t root;
uint64_t vmm_kernel_root(void) { return root; }
int irq_in_handler(void) { return in_irq; }
const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi,uint32_t i)
{ return (const void *)((const uint8_t *)bi->mmap+sizeof(*bi->mmap)+i*bi->mmap->entry_size); }
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr,"unexpected panic: %s\n",s); abort(); }

static char out[8192];
static size_t outlen;
static void emit(const char *s) { size_t n=strlen(s); assert(outlen+n<sizeof(out)); memcpy(out+outlen,s,n); outlen+=n; out[outlen]=0; }
void serial_puts(const char *s) { emit(s); }
void serial_hex(uint64_t v) { char b[24]; snprintf(b,sizeof b,"0x%llx",(unsigned long long)v); emit(b); }
void serial_dec(uint64_t v) { char b[24]; snprintf(b,sizeof b,"%llu",(unsigned long long)v); emit(b); }

static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n%s\n",__FILE__,__LINE__,#x,out);_exit(1);} } while(0)
static uint64_t base;

static void setup(void)
{
    static struct { struct mb2_tag_mmap header; struct mb2_mmap_entry entries[1]; } mm = {
        .header={.entry_size=sizeof(struct mb2_mmap_entry)},
        .entries={{.base=0x100000,.length=0x100000,.type=MB2_MMAP_AVAILABLE}}
    };
    static struct boot_info bi={.mmap=&mm.header,.mmap_entries=1};
    CHECK(pmm_init(&bi,NULL)==0);
    CHECK(pmm_alloc_frame(&root)==0);
    cr3=root;
    base=pmm_free_count();
}
static int run(const char *cmdline) { outlen=0; out[0]=0; return usb_dma_boot(cmdline); }

/* Run one scenario in a child with fresh Rust and C state. */
static void scenario(const char *name, void (*body)(void))
{
    fflush(stdout);
    pid_t pid=fork();
    if (!pid) { body(); printf("  scenario %s: %u checks PASS\n",name,checks); fflush(stdout); _exit(0); }
    int status=0;
    assert(waitpid(pid,&status,0)==pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr,"scenario %s FAILED\n",name); exit(1); }
}

static void clean(void)
{
    CHECK(run("")==1 && !outlen && !held.count && pmm_free_count()==base);
    CHECK(run("vos.usb2c2-dma=1")==1 && strstr(out,"usb2c2: PASS"));
    /* 4 (set) + 0+1+2+3 (injection rounds) + 1 (transition) + 1 (quarantine). */
    CHECK(strstr(out,"allocated=0xc") && strstr(out,"prepared=0xc") && strstr(out,"released=0xb"));
    CHECK(strstr(out,"quarantined=0x1") && strstr(out,"retained_now=0x1") && strstr(out,"active_now=0x0"));
    CHECK(strstr(out,"allocation_failures=0x5") && strstr(out,"violations=0x2") && strstr(out,"rollback_failures=0x0"));
    CHECK(strstr(out,"injected_steps=0x4") && strstr(out,"retained_state=0x5") && strstr(out,"frames_held=0x1"));
    CHECK(held.count==1 && pmm_free_count()==base-1 && pmm_check_in(pmm_kernel())==PMM_OK);
    /* The retained frame stays owned: never freed, and the pool cannot run again. */
    CHECK(run("vos.usb2c2-dma=1")==0 && strstr(out,"repeated run") && held.count==1 && pmm_free_count()==base-1);
}
static void zero_fault(void)
{
    usb_dma_faults(-1,0,-1);
    CHECK(run("vos.usb2c2-dma=1")==0 && strstr(out,"FAIL: stage 1"));
    CHECK(strstr(out,"retained_now=0x1") && strstr(out,"active_now=0x0") && strstr(out,"quarantined=0x1"));
    CHECK(held.count==1 && pmm_free_count()==base-1);   /* unzeroed frame kept, never reused */
}
static void free_fault(void)
{
    usb_dma_faults(-1,-1,0);
    CHECK(run("vos.usb2c2-dma=1")==0 && strstr(out,"FAIL: stage 3"));
    CHECK(strstr(out,"rollback_failures=0x1") && strstr(out,"retained_now=0x1") && strstr(out,"released=0x3"));
    CHECK(held.count==1 && pmm_free_count()==base-1);
}
static void alloc_fault(void)
{
    usb_dma_faults(2,-1,-1);
    CHECK(run("vos.usb2c2-dma=1")==0 && strstr(out,"FAIL: stage 1"));
    CHECK(strstr(out,"released=0x2") && strstr(out,"retained_now=0x0") && strstr(out,"allocation_failures=0x1"));
    CHECK(held.count==0 && pmm_free_count()==base);    /* exact rollback */
}
static void bad_context(void)
{
    flags=RFLAGS_IF;
    CHECK(run("vos.usb2c2-dma=1")==0 && strstr(out,"FAIL: stage 1") && !held.count && pmm_free_count()==base);
}
static void bad_report(void)
{
    _Alignas(8) unsigned char raw[sizeof(struct usb_dma_report)+8];
    CHECK(vos_usb_dma_exercise_v1(NULL)==-2);
    CHECK(vos_usb_dma_exercise_v1((void *)(raw+4))==-2);
    CHECK(run("vos.usb2c2-dma=1")==1 && held.count==1);   /* refusals did not consume the run */
}

int main(void)
{
    setup();
    /* Direct adapter tests (no Rust): allow-list, zeroing, injection, context. */
    uint64_t f[USB_DMA_FRAMES+1];
    CHECK(usb_dma_frame_alloc(NULL)==USB_DMA_EINVAL);
    for (unsigned i=0;i<USB_DMA_FRAMES;i++) {
        CHECK(usb_dma_frame_alloc(&f[i])==0 && f[i]>=0x100000 && !(f[i]&0xfff));
        memset(phys_to_virt(f[i]),0xa5,PAGE_SIZE);
    }
    CHECK(usb_dma_frame_alloc(&f[USB_DMA_FRAMES])==USB_DMA_ESTATE && !f[USB_DMA_FRAMES]);
    CHECK(pmm_free_count()==base-USB_DMA_FRAMES);
    uint64_t foreign; CHECK(pmm_alloc_frame(&foreign)==0);
    CHECK(usb_dma_frame_zero(foreign)==USB_DMA_ESTATE && usb_dma_frame_free(foreign)==USB_DMA_ESTATE);
    CHECK(pmm_free_frame(foreign)==0);
    for (unsigned i=0;i<USB_DMA_FRAMES;i++) {
        CHECK(usb_dma_frame_zero(f[i])==0);
        const unsigned char *p=phys_to_virt(f[i]);
        for (unsigned b=0;b<PAGE_SIZE;b++) CHECK(!p[b]);
    }
    for (int c=0;c<3;c++) {
        cr3=c==0?root+4096:root; flags=c==1?RFLAGS_IF:0; in_irq=c==2;
        uint64_t x; CHECK(usb_dma_frame_alloc(&x)==USB_DMA_ESTATE && !x);
        CHECK(usb_dma_frame_zero(f[0])==USB_DMA_ESTATE && usb_dma_frame_free(f[0])==USB_DMA_ESTATE);
        cr3=root; flags=0; in_irq=0;
    }
    for (int at=0;at<3;at++) {               /* injected failure at each index, then success */
        usb_dma_faults(-1,at,at);
        for (int n=0;n<at;n++) CHECK(usb_dma_frame_zero(f[n])==0);
        CHECK(usb_dma_frame_zero(f[at])==USB_DMA_EFAULT);
        usb_dma_faults(-1,-1,-1);
    }
    usb_dma_faults(-1,-1,1);
    CHECK(usb_dma_frame_free(f[0])==0 && usb_dma_frame_free(f[1])==USB_DMA_EFAULT && held.count==USB_DMA_FRAMES-1);
    usb_dma_faults(-1,-1,-1);
    CHECK(usb_dma_frame_free(f[0])==USB_DMA_ESTATE); /* double free refused */
    for (unsigned i=1;i<USB_DMA_FRAMES;i++) CHECK(usb_dma_frame_free(f[i])==0);
    CHECK(!held.count && pmm_free_count()==base && pmm_check_in(pmm_kernel())==PMM_OK);
    usb_dma_faults(0,-1,-1);
    uint64_t y; CHECK(usb_dma_frame_alloc(&y)==USB_DMA_EFAULT && !y && pmm_free_count()==base);
    usb_dma_faults(-1,-1,-1);
    printf("adapter: %u checks PASS\n",checks);

    scenario("clean pass, retention and repeat refusal", clean);
    scenario("zeroing failure retains the frame", zero_fault);
    scenario("release failure retains the frame", free_fault);
    scenario("allocation failure rolls back exactly", alloc_fault);
    scenario("bad context refuses everything", bad_context);
    scenario("null and misaligned report", bad_report);
    printf("PASS: real PMM + USB DMA adapter C with the bare-metal Rust pool/exercise: allow-list, zeroing, "
           "injection, context, exact rollback, zero/free-failure retention, once-per-boot\n");
#ifdef USB2C2_BENCH
    /* Optimized, unsanitized timing of the adapter cycle with real PMM work. */
    for (unsigned sample=0;sample<7;sample++) {
        struct timespec a,b; uint64_t x;
        clock_gettime(CLOCK_MONOTONIC,&a);
        for (unsigned n=0;n<100000;n++) {
            if (usb_dma_frame_alloc(&x) || usb_dma_frame_zero(x) || usb_dma_frame_free(x)) return 1;
        }
        clock_gettime(CLOCK_MONOTONIC,&b);
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000ULL+b.tv_nsec-a.tv_nsec;
        printf("sample=%u ns_per_alloc_zero_free=%llu\n",sample,(unsigned long long)(ns/100000));
    }
#endif
    return 0;
}

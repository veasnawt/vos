/* Hosted timing harness; no production code changes, QEMU timing or sanitizer. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "elf.h"

typedef int (*predicate)(uint64_t,uint64_t,uint64_t);
typedef int (*validator)(const uint8_t *,uint64_t,struct elf_image *);
int bench_c_inside(uint64_t,uint64_t,uint64_t);
int bench_call_control(uint64_t,uint64_t,uint64_t);
int vos_rust_elf_inside(uint64_t,uint64_t,uint64_t);
int bench_elf_c(const uint8_t *,uint64_t,struct elf_image *);
int bench_elf_rust(const uint8_t *,uint64_t,struct elf_image *);

struct vector { uint64_t size,off,len; };
struct input {uint8_t *bytes;uint64_t size;};
static volatile uint64_t sink;
static uint64_t state;
static uint64_t random64(void) {state=state*UINT64_C(6364136223846793005)+1;return state;}
__attribute__((noreturn)) void panic(const char *message)
{fprintf(stderr,"unexpected Rust panic: %s\n",message);exit(2);}
static uint64_t clock_ns(clockid_t id)
{struct timespec t;if(clock_gettime(id,&t))exit(2);return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;}

/* Separate non-specialized loops: actual indirect calls for both ABI variants.
 * Volatile input reads prevent repeated vectors being hoisted to constants;
 * the final checksum is consumed outside the timed loop. */
__attribute__((noinline,noclone))
uint64_t bench_predicate_abi(predicate fn,volatile const struct vector *v,uint64_t n)
{
    uint64_t sum=0;
    for(uint64_t i=0;i<n;i++) {
        uint64_t s=v[i&255].size,o=v[i&255].off,l=v[i&255].len;
        sum+=(unsigned)fn(s,o,l);
    }
    return sum;
}
__attribute__((noinline,noclone))
uint64_t bench_predicate_inline(volatile const struct vector *v,uint64_t n)
{
    uint64_t sum=0;
    for(uint64_t i=0;i<n;i++) {
        uint64_t s=v[i&255].size,o=v[i&255].off,l=v[i&255].len;
        sum+=(o<=s && l<=s-o);
    }
    return sum;
}
__attribute__((noinline,noclone))
uint64_t bench_validate(validator fn,const struct input *v,uint64_t n)
{
    uint64_t sum=0;struct elf_image image;
    for(uint64_t i=0;i<n;i++) {
        int rc=fn(v[i&7].bytes,v[i&7].size,&image);
        sum+=(unsigned)rc+image.entry+image.pages+image.abi;
    }
    return sum;
}

static int image_equal(const struct elf_image *a,const struct elf_image *b)
{
    if(a->entry!=b->entry || a->abi!=b->abi || a->pages!=b->pages || a->nload!=b->nload)return 0;
    for(unsigned i=0;i<ELF_MAX_LOADS;i++) {
        const struct elf_segment *x=&a->load[i],*y=&b->load[i];
        if(x->vaddr!=y->vaddr || x->offset!=y->offset || x->filesz!=y->filesz ||
           x->memsz!=y->memsz || x->flags!=y->flags)return 0;
    }
    return 1;
}
static void emit(const char *comparison,const char *name,unsigned sample,uint64_t n,
                 uint64_t wall,uint64_t cpu,uint64_t sum,const struct rusage *a,const struct rusage *b)
{
    sink^=sum;
    printf("%s,%s,%u,%llu,%llu,%llu,%llu,%ld,%ld\n",comparison,name,sample,
           (unsigned long long)n,(unsigned long long)wall,(unsigned long long)cpu,
           (unsigned long long)sum,b->ru_nvcsw-a->ru_nvcsw,b->ru_nivcsw-a->ru_nivcsw);
}
int main(int argc,char **argv)
{
    if(argc!=5){fprintf(stderr,"usage: bench SEED SAMPLES SCALAR_OPS CORPUS_LIST\n");return 2;}
    state=strtoull(argv[1],0,0);unsigned samples=(unsigned)strtoul(argv[2],0,0);
    uint64_t n=strtoull(argv[3],0,0);if(samples<3 || samples>31 || n<256 || n>(1U<<26))return 2;
    struct vector v[256];
    const uint64_t edge[]={0,1,2,63,64,65,4095,4096,UINT64_MAX-1,UINT64_MAX};
    unsigned valid=0;
    for(unsigned i=0;i<256;i++) {
        uint64_t s=4096+(random64()&65535),o=random64()%(s+1),l=random64()%(s-o+1);
        switch(i&7) {
        case 1:o=s+1+(random64()&31);l=0;break;
        case 2:s=edge[(i/8)%10];o=edge[(i/16)%10];l=edge[(i/32)%10];break;
        case 3:o=s;l=0;break;
        case 4:l=s-o+1;break;
        case 5:s=UINT64_MAX-2;o=UINT64_MAX-3;l=8;break;
        case 6:l=0;break;
        case 7:s=0;o=UINT64_MAX;l=0;break;
        default:break;
        }
        v[i]=(struct vector){s,o,l};
    }
    /* Shuffle input order once; no random generation in a timed region. */
    for(unsigned i=255;i;i--){unsigned j=(unsigned)(random64()%(i+1));struct vector x=v[i];v[i]=v[j];v[j]=x;}
    for(unsigned i=0;i<256;i++) {
        int c=bench_c_inside(v[i].size,v[i].off,v[i].len);
        int r=vos_rust_elf_inside(v[i].size,v[i].off,v[i].len);
        int wide=(__uint128_t)v[i].off+v[i].len<=(__uint128_t)v[i].size;
        if(c!=r || c!=wide || (r!=0 && r!=1))return 1;
        valid+=(unsigned)c;
    }
    struct input inputs[4][8];FILE *list=fopen(argv[4],"r");if(!list)return 2;
    for(unsigned g=0;g<4;g++)for(unsigned i=0;i<8;i++) {
        char path[4096];if(!fgets(path,sizeof(path),list))return 2;path[strcspn(path,"\n")]=0;
        FILE *f=fopen(path,"rb");if(!f || fseek(f,0,SEEK_END))return 2;
        long size=ftell(f);if(size<0 || (unsigned long)size>ELF_MAX_FILE || fseek(f,0,SEEK_SET))return 2;
        uint8_t *bytes=malloc(size?(size_t)size:1);if(!bytes || fread(bytes,1,(size_t)size,f)!=(size_t)size || fclose(f))return 2;
        inputs[g][i]=(struct input){bytes,(uint64_t)size};struct elf_image a,b;
        int c=bench_elf_c(bytes,(uint64_t)size,&a),r=bench_elf_rust(bytes,(uint64_t)size,&b);
        if(c!=r || !image_equal(&a,&b) || (g==0 && c!=ELF_OK) || (g==1 && c!=ELF_EPHBOUNDS) || (g==2 && c==ELF_OK))return 1;
    }
    fclose(list);
    fprintf(stderr,"preflight: 256 scalar vectors (%u valid, %u invalid), 32 ELF positions; exact C/Rust agreement\n",valid,256-valid);
    puts("comparison,implementation,sample,operations,wall_ns,cpu_ns,checksum,voluntary_switches,involuntary_switches");
    const char *names[]={"C_ABI","Rust_ABI","C_inline","call_control"};
    predicate fns[]={bench_c_inside,vos_rust_elf_inside,0,bench_call_control};
    uint64_t expected=bench_predicate_abi(bench_c_inside,v,n);
    if(bench_predicate_inline(v,n)!=expected)return 1;
    for(unsigned round=0;round<samples;round++)for(unsigned k=0;k<4;k++) {
        unsigned mode=(k+round)%4;struct rusage a,b;
        if(mode==2)sink^=bench_predicate_inline(v,n/16);else sink^=bench_predicate_abi(fns[mode],v,n/16);
        if(getrusage(RUSAGE_THREAD,&a))return 2;
        uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID),wall=clock_ns(CLOCK_MONOTONIC_RAW);
        uint64_t sum=mode==2?bench_predicate_inline(v,n):bench_predicate_abi(fns[mode],v,n);
        wall=clock_ns(CLOCK_MONOTONIC_RAW)-wall;cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        if(getrusage(RUSAGE_THREAD,&b))return 2;
        if(mode!=3 && sum!=expected)return 1;
        emit("scalar_mixed",names[mode],round,n,wall,cpu,sum,&a,&b);
    }
    const char *groups[]={"ELF_valid","ELF_bounds_reject","ELF_other_reject","ELF_mixed"};
    const uint64_t count=65536;
    for(unsigned round=0;round<samples;round++)for(unsigned k=0;k<4;k++) {
        unsigned g=(k+round)%4;uint64_t sums[2];
        for(unsigned order=0;order<2;order++) {
            unsigned mode=(order+round)%2;validator fn=mode?bench_elf_rust:bench_elf_c;
            sink^=bench_validate(fn,inputs[g],count/16);struct rusage a,b;
            if(getrusage(RUSAGE_THREAD,&a))return 2;
            uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID),wall=clock_ns(CLOCK_MONOTONIC_RAW);
            sums[mode]=bench_validate(fn,inputs[g],count);
            wall=clock_ns(CLOCK_MONOTONIC_RAW)-wall;cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
            if(getrusage(RUSAGE_THREAD,&b))return 2;
            emit(groups[g],mode?"Rust_integrated":"C_original",round,count,wall,cpu,sums[mode],&a,&b);
        }
        if(sums[0]!=sums[1])return 1;
    }
    for(unsigned g=0;g<4;g++)for(unsigned i=0;i<8;i++)free(inputs[g][i].bytes);
    fprintf(stderr,"consumed checksum=%llu\n",(unsigned long long)sink);return 0;
}

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "app_catalog.h"
#include "elf.h"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
struct guarded { uint64_t before; struct app_catalog catalog; uint64_t after; };
static struct guarded guard;
static struct app_source source[APP_CATALOG_MAX];
static uint8_t *data[3];
static size_t lengths[3];
static const char *names[3] = { "vsh", "hello", "rixie" };
static const unsigned policies[3] = { APP_POLICY_SHELL, APP_POLICY_DIAGNOSTIC, APP_POLICY_CONSOLE };
static void valid(void)
{
    memset(source,0,sizeof(source));
    for (unsigned i=0;i<3;i++) {
        source[i].version=APP_SOURCE_VERSION;source[i].policy=policies[i];source[i].identity=0x1001+i;
        strcpy(source[i].name,names[i]);source[i].image=data[i];source[i].length=lengths[i];
    }
}
static void canaries(void) { CHECK(guard.before==UINT64_C(0x1bafeed123456789));CHECK(guard.after==UINT64_C(0x9badf00d87654321)); }
static void rejected(int expected,size_t count)
{
    memset(&guard.catalog,0xa5,sizeof(guard.catalog));
    CHECK(app_catalog_build(&guard.catalog,source,count)==expected);
    const uint8_t *p=(const uint8_t *)&guard.catalog;
    for (size_t i=0;i<sizeof(guard.catalog);i++) CHECK(p[i]==0);
    canaries();CHECK(app_catalog_at(&guard.catalog,0)==NULL);CHECK(app_catalog_find(&guard.catalog,0x1001)==NULL);
}
static uint8_t *read_file(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);CHECK(fseek(f,0,SEEK_END)==0);
    long n=ftell(f);CHECK(n>0 && (unsigned long)n<=ELF_MAX_FILE);rewind(f);
    uint8_t *p=malloc((size_t)n);CHECK(p!=NULL);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);
    CHECK(fclose(f)==0);*size=(size_t)n;return p;
}
static uint16_t u16(const uint8_t *p) {return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p) {return (uint32_t)u16(p)|(uint32_t)u16(p+2)<<16;}
static uint64_t u64(const uint8_t *p) {return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static uint64_t ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv)
{
    CHECK(argc==4 || argc==5);
    for(unsigned i=0;i<3;i++)data[i]=read_file(argv[i+1],&lengths[i]);
    guard.before=UINT64_C(0x1bafeed123456789);guard.after=UINT64_C(0x9badf00d87654321);valid();
    if(argc==5) {
        CHECK(!strcmp(argv[4],"--bench"));
        uint64_t samples[7];
        for(unsigned s=0;s<7;s++) {
            uint64_t start=ns();
            for(unsigned j=0;j<10000;j++) if(app_catalog_build(&guard.catalog,source,3))return 2;
            samples[s]=ns()-start;
        }
        printf("{\"iterations_per_sample\":10000,\"catalog_bytes\":%zu,\"source_bytes\":%zu,\"sample_total_ns\":[",sizeof(guard.catalog),sizeof(source[0]));
        for(unsigned s=0;s<7;s++)printf("%s%llu",s?",":"",(unsigned long long)samples[s]);
        puts("]}");for(unsigned i=0;i<3;i++)free(data[i]);return 0;
    }
    CHECK(sizeof(struct app_catalog)==392);CHECK(sizeof(struct app_catalog_entry)==48);
    CHECK(app_catalog_build(NULL,source,3)==APP_CATALOG_EINVAL);
    CHECK(app_catalog_build(&guard.catalog,NULL,3)==APP_CATALOG_EINVAL);canaries();
    rejected(APP_CATALOG_EINVAL,0);rejected(APP_CATALOG_ECAPACITY,APP_CATALOG_MAX+1);
    CHECK(app_catalog_build(&guard.catalog,source,3)==APP_CATALOG_OK);canaries();CHECK(guard.catalog.count==3);
    for(unsigned i=0;i<3;i++) {
        const struct app_catalog_entry *e=app_catalog_at(&guard.catalog,i);
        CHECK(e!=NULL);CHECK(e==app_catalog_find(&guard.catalog,0x1001+i));
        CHECK(!strcmp(e->name,names[i]));CHECK(e->image==data[i] && e->length==lengths[i]);
        CHECK(i==0 ? (e->abi==3 || e->abi==4) : e->abi==(i==1?0U:3U));CHECK(e->policy==policies[i]);
    }
    CHECK(app_catalog_at(&guard.catalog,3)==NULL);CHECK(app_catalog_at(&guard.catalog,SIZE_MAX)==NULL);
    CHECK(app_catalog_find(&guard.catalog,0)==NULL);CHECK(app_catalog_find(&guard.catalog,999)==NULL);
    CHECK(app_catalog_at(NULL,0)==NULL);CHECK(app_catalog_find(NULL,1)==NULL);
    for(unsigned i=0;i<APP_CATALOG_MAX;i++) {
        source[i]=source[1];source[i].identity=100+i;
        memset(source[i].name,0,sizeof(source[i].name));snprintf(source[i].name,sizeof(source[i].name),"app%u",i);
    }
    CHECK(app_catalog_build(&guard.catalog,source,APP_CATALOG_MAX)==0);CHECK(guard.catalog.count==APP_CATALOG_MAX);
    for(unsigned i=0;i<APP_CATALOG_MAX;i++)CHECK(app_catalog_find(&guard.catalog,100+i)==app_catalog_at(&guard.catalog,i));
    CHECK(app_catalog_at(&guard.catalog,APP_CATALOG_MAX)==NULL);
    guard.catalog.count=UINT32_MAX;CHECK(app_catalog_at(&guard.catalog,0)==NULL);CHECK(app_catalog_find(&guard.catalog,100)==NULL);
    valid();source[1].identity=source[0].identity;rejected(APP_CATALOG_EDUPLICATE,3);
    valid();memcpy(source[1].name,source[0].name,sizeof(source[1].name));rejected(APP_CATALOG_EDUPLICATE,3);
    const char *bad_names[]={"","help","clear","echo","pwd","ls","cd","cat","exit","apps","a/b","a b",".","..","UPPER","a\tb"};
    for(unsigned i=0;i<sizeof(bad_names)/sizeof(bad_names[0]);i++) {
        valid();memset(source[0].name,0,sizeof(source[0].name));strcpy(source[0].name,bad_names[i]);rejected(APP_CATALOG_ENAME,3);
    }
    valid();memset(source[0].name,'x',sizeof(source[0].name));rejected(APP_CATALOG_ENAME,3);
    valid();source[0].name[0]=(char)0xff;rejected(APP_CATALOG_ENAME,3);
    valid();source[0].name[10]='x';rejected(APP_CATALOG_ENAME,3);
    valid();memset(source[1].name,'x',APP_CATALOG_NAME_MAX);source[1].name[APP_CATALOG_NAME_MAX]=0;
    CHECK(app_catalog_build(&guard.catalog,source,3)==0);CHECK(strlen(app_catalog_at(&guard.catalog,1)->name)==15);
    for(unsigned i=0;i<3;i++) {
        valid();source[i].identity=0;rejected(APP_CATALOG_EINVAL,3);
        valid();source[i].version=0;rejected(APP_CATALOG_EINVAL,3);
        valid();source[i].version=2;rejected(APP_CATALOG_EINVAL,3);
        valid();source[i].policy=0;rejected(APP_CATALOG_EPOLICY,3);
        valid();source[i].policy=UINT32_MAX;rejected(APP_CATALOG_EPOLICY,3);
        valid();source[i].image=NULL;rejected(APP_CATALOG_EINVAL,3);
        valid();source[i].length=ELF_MAX_FILE+1;rejected(APP_CATALOG_EINVAL,3);
        valid();source[i].image=(const uint8_t *)(UINTPTR_MAX-8);source[i].length=64;rejected(APP_CATALOG_EINVAL,3);
    }
    valid();source[1].policy=APP_POLICY_CONSOLE;rejected(APP_CATALOG_EPOLICY,3);
    valid();source[1].policy=APP_POLICY_SHELL;rejected(APP_CATALOG_EPOLICY,3);
    for(unsigned i=0;i<3;i++)for(unsigned n=0;n<64;n++) {
        valid();source[i].length=n;rejected(n?APP_CATALOG_EEXEC:APP_CATALOG_EINVAL,3);
    }
    uint8_t *mutated=malloc(lengths[1]);CHECK(mutated!=NULL);
    for(unsigned variant=0;variant<4;variant++) {
        memcpy(mutated,data[1],lengths[1]);
        if(variant==0)mutated[0]=0;
        else if(variant==1)mutated[16]=3;
        else {
            uint64_t ph=u64(mutated+32);unsigned count=u16(mutated+56),changed=0;
            for(unsigned i=0;i<count;i++) {
                uint8_t *p=mutated+ph+56*i;uint32_t type=u32(p),flags=u32(p+4);
                if(variant==2 && type==1 && (flags&1)) {put32(p+4,7);changed=1;break;}
                if(variant==3 && type==4) {put32(mutated+u64(p+8)+16,ELF_ABI_MAX+1);changed=1;break;}
            }
            CHECK(changed);
        }
        valid();source[1].image=mutated;rejected(APP_CATALOG_EEXEC,3);
    }
    free(mutated);valid();CHECK(app_catalog_build(&guard.catalog,source,3)==0);canaries();
    for(unsigned i=0;i<3;i++)free(data[i]);
    printf("app-catalog: %u checks PASS; catalog=%zu bytes; max=%u; no execution or grants\n",checks,sizeof(guard.catalog),APP_CATALOG_MAX);
    return 0;
}

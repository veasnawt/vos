#include <vos/file.h>
#include <stddef.h>
static unsigned char buffer[8192];
static const char ro[] = "protected";
static unsigned checks, failed;
static void check(int yes) { checks++; failed += !yes; }
static void fill(unsigned char *p, unsigned n) { for(unsigned i=0;i<n;i++) p[i]=0xa5; }
static int untouched(const unsigned char *p, unsigned n) {
    for(unsigned i=0;i<n;i++) if(p[i]!=0xa5) return 0;
    return 1;
}
static int same(const void *a,const void *b,unsigned n) {
    const unsigned char *x=a,*y=b;
    for(unsigned i=0;i<n;i++) if(x[i]!=y[i]) return 0;
    return 1;
}
static void bytes_to(void *to,const void *from,unsigned n) {
    unsigned char *dst=to;const unsigned char *src=from;
    for(unsigned i=0;i<n;i++) dst[i]=src[i];
}
int main(void)
{
    long h=vos_file_initial(0), f;
    check(h>0 && vos_file_initial(0)==h);
    check(vos_file_initial(2)==-VOS_EINVAL);
    check(vos_file_initial(1)==-VOS_EBADHANDLE);
    check(vos_file_stat(h+1,buffer,64)==-VOS_EBADHANDLE); /* foreign grant in trusted runner */
    check(vos_channel_send(h,0,0)==-VOS_EBADHANDLE);
    check(vos_handle_close(0)==-VOS_EBADHANDLE);
    check(vos_file_open_at(1,".",1,0)==-VOS_EBADHANDLE);
    check(vos_handle_close(1)==0 && vos_handle_close(1)==-VOS_EBADHANDLE);
    for(unsigned bit=4;bit<64;bit++) {
        uint64_t bad=UINT64_C(1)<<bit;
        check(vos_file_open_at(h,".",1,bad)==-VOS_EINVAL);
        check(vos_file_open_at(h,".",1,bad|VOS_FILE_READ)==-VOS_EINVAL);
        check(vos_file_open_at(h,".",1,bad|VOS_FILE_RIGHTS)==-VOS_EINVAL);
    }
    check(vos_file_open_at(h,"/x",2,0)==-VOS_EINVAL);
    check(vos_file_open_at(h,"missing/../x",12,0)==-VOS_EACCES);
    check(vos_file_open_at(h,"x/..",4,0)==-VOS_EACCES);
    check(vos_file_open_at(h,"x//name.txt",11,0)==-VOS_EINVAL);
    check(vos_file_open_at(h,"x/",2,0)==-VOS_EINVAL);
    check(vos_file_open_at(h,"x\0a",3,0)==-VOS_EINVAL);
    check(vos_file_open_at(h,"X/name.txt",10,0)==-VOS_ENOENT);
    check(vos_file_open_at(h,0,0,0)==-VOS_EINVAL);
    check(vos_file_open_at(h,0,1025,0)==-VOS_ENAMETOOLONG);
    check(vos_file_open_at(h,0,1,16)==-VOS_EINVAL);
    check(vos_file_open_at(h,0,1,0)==-VOS_EFAULT);
    check(vos_file_open_at(h,(void *)-1,2,0)==-VOS_EFAULT);
    check(vos_file_open_at(h,(void *)0xFFFFFFFF80000000UL,2,0)==-VOS_EFAULT);
    f=vos_file_open_at(h,"x/name.txt",10,VOS_FILE_READ|VOS_FILE_STAT);
    check(f>0);
    const char text[]="name.txt in /dup/x\n";
    fill(buffer,sizeof(buffer));
    check(vos_file_read_at(f,buffer,4096,0)==sizeof(text)-1 && same(buffer,text,sizeof(text)-1));
    check(untouched(buffer+sizeof(text)-1,sizeof(buffer)-(sizeof(text)-1)));
    fill(buffer,sizeof(buffer));
    check(vos_file_read_at(f,buffer,5,3)==5 && same(buffer,text+3,5) && untouched(buffer+5,8192-5));
    fill(buffer,sizeof(buffer));
    check(vos_file_read_at(f,buffer,4096,4096)==0 && untouched(buffer,sizeof(buffer)));
    check(vos_file_read_at(f,0,0,UINT64_MAX)==0);
    check(vos_file_read_at(f,buffer,4097,0)==-VOS_EINVAL);
    check(vos_file_read_at(f,buffer,2,UINT64_MAX)==-VOS_EINVAL);
    check(vos_file_read_at(f,0,1,0)==-VOS_EFAULT);
    check(vos_file_read_at(f,(void *)ro,1,0)==-VOS_EFAULT);
    check(vos_file_read_at(f,(void *)ro,1,4096)==-VOS_EFAULT);
    check(vos_file_read_at(f,(void *)main,1,0)==-VOS_EFAULT);
    check(vos_file_read_at(f,(void *)0xFFFFFFFF80000000UL,1,0)==-VOS_EFAULT);
    check(vos_file_read_at(f,(void *)0x800000000000UL,1,0)==-VOS_EFAULT);
    check(vos_file_read_at(f,(void *)(UINT64_MAX-1),4,0)==-VOS_EFAULT);
    volatile unsigned char *edge=(void *)UINT64_C(0x7fffffffeffc);
    for(unsigned i=0;i<4;i++) edge[i]=0xa5;
    check(vos_file_read_at(f,(void *)edge,8,0)==-VOS_EFAULT);
    check(vos_file_stat(f,(void *)edge,64)==-VOS_EFAULT);
    check(edge[0]==0xa5 && edge[1]==0xa5 && edge[2]==0xa5 && edge[3]==0xa5);
    check(vos_file_read_at(h,0,0,0)==-VOS_EISDIR);
    check(vos_file_stat(f,buffer+1,64)==0);
    /* Exercise an unaligned wire destination without a misaligned C lvalue. */
    struct vos_file_stat aligned_stat,*st=&aligned_stat;
    bytes_to(st,buffer+1,sizeof(*st));
    check(st->size==sizeof(text)-1 && st->kind==1 && st->rights==3);
    int zero=1;for(unsigned i=0;i<24;i++) zero &= st->reserved[i]==0;check(zero);
    fill(buffer,sizeof(buffer));
    check(vos_file_stat(f,buffer,63)==-VOS_EINVAL && untouched(buffer,sizeof(buffer)));
    check(vos_file_stat(f,buffer,UINT64_C(64)+(UINT64_C(1)<<32))==-VOS_EINVAL);
    check(vos_file_stat(f,(void *)ro,64)==-VOS_EFAULT);
    check(vos_file_stat(f,(void *)(UINT64_MAX-1),64)==-VOS_EFAULT);
    check(vos_file_open_at(f,".",1,0)==-VOS_ENOTDIR);
    check(vos_directory_next(f,0,buffer,272)==-VOS_ENOTDIR);
    long n=vos_file_open_at(h,"x",1,VOS_FILE_TRAVERSE);
    check(n>0 && vos_file_open_at(n,"name.txt",8,VOS_FILE_READ)==-VOS_EACCES);
    check(vos_file_stat(n,buffer,64)==-VOS_EACCES);
    check(vos_directory_next(n,0,buffer,272)==-VOS_EACCES);
    check(vos_handle_close(n)==0);
    n=vos_file_open_at(h,"x/name.txt",10,VOS_FILE_STAT);
    check(n>0 && vos_file_read_at(n,0,0,0)==-VOS_EACCES && vos_handle_close(n)==0);
    n=vos_file_open_at(h,"x",1,VOS_FILE_ENUMERATE|VOS_FILE_STAT);
    check(n>0 && vos_directory_next(n,1,buffer,272)==-VOS_EINVAL);
    check(vos_directory_next(n,UINT64_MAX,buffer,272)==-VOS_EINVAL);
    check(vos_directory_next(n,0,buffer,271)==-VOS_EINVAL);
    check(vos_directory_next(n,0,buffer,UINT64_C(272)+(UINT64_C(1)<<32))==-VOS_EINVAL);
    check(vos_directory_next(n,0,(void *)edge,272)==-VOS_EFAULT);
    check(edge[0]==0xa5 && edge[1]==0xa5 && edge[2]==0xa5 && edge[3]==0xa5);
    check(vos_directory_next(n,0,(void *)ro,272)==-VOS_EFAULT);
    check(vos_directory_next(n,24,buffer+1,272)==1);
    struct vos_directory_entry aligned_entry,*e=&aligned_entry;
    bytes_to(e,buffer+1,sizeof(*e));
    check(e->kind==1 && e->name_length==8 && same(e->name,"name.txt",8));
    zero=1;for(unsigned i=8;i<256;i++) zero &= e->name[i]==0;check(zero);
    uint64_t cookie=e->next_cookie;
    fill(buffer,sizeof(buffer));
    check(vos_directory_next(n,cookie,buffer,272)==0 && untouched(buffer,sizeof(buffer)));
    check(vos_directory_next(n,cookie,(void *)ro,272)==-VOS_EFAULT);
    check(vos_handle_close(n)==0 && vos_handle_close(n)==-VOS_EBADHANDLE);
    long handles[6];for(unsigned i=0;i<6;i++){handles[i]=vos_file_open_at(h,".",1,0);check(handles[i]>0);}
    check(vos_file_open_at(h,".",1,0)==-VOS_ENOMEM);
    for(unsigned i=0;i<6;i++) check(vos_handle_close(handles[i])==0);
    check(vos_handle_close(h)==0 && vos_file_initial(0)==-VOS_EBADHANDLE);
    check(vos_file_read_at(f,buffer,4096,0)==sizeof(text)-1);
    check(vos_handle_close(f)==0 && vos_file_read_at(f,0,0,0)==-VOS_EBADHANDLE);
    return (int)failed;
}

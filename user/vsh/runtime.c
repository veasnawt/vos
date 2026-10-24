/* Freestanding memory support for Rust-generated calls; no host libc. */
#include <stddef.h>
void *memcpy(void *d,const void *s,size_t n) { unsigned char *a=d; const unsigned char *b=s; for(size_t i=0;i<n;i++) a[i]=b[i]; return d; }
void *memset(void *d,int c,size_t n) { unsigned char *a=d; for(size_t i=0;i<n;i++) a[i]=(unsigned char)c; return d; }
void *memmove(void *d,const void *s,size_t n) { unsigned char *a=d; const unsigned char *b=s; if(a<b) return memcpy(d,s,n); while(n) { n--;a[n]=b[n]; } return d; }
int memcmp(const void *a,const void *b,size_t n) { const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0; }

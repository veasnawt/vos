#include <vos/file.h>
/* Kernel selects an immutable overlay; expected error is checked there. */
int main(void)
{
    long h=vos_file_initial(0);
    long a=vos_file_open_at(h,"x",1,0);
    long b=vos_file_open_at(h,"x/name.txt",10,0);
    return a<0 && a==b ? (int)-a : 99;
}

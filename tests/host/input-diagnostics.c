#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user_console.h"
static char logtext[256];
uint8_t console_host_in(uint16_t port) { (void)port; abort(); }
void console_host_out(uint16_t port, uint8_t data) { (void)port; (void)data; abort(); }
void serial_puts(const char *s) {
    if(strlen(logtext)+strlen(s)>=sizeof(logtext))abort();
    strcat(logtext,s);
}
int main(void) {
    user_console_diagnostic("PS/2",0);
    if(*logtext)return 1;
    user_console_set_diagnostics(1);
    user_console_diagnostic("PS/2",0);
    user_console_diagnostic("ELF",1);
    if(strcmp(logtext,"input-diag: PS/2 failed\r\ninput-diag: ELF ok\r\n"))return 1;
    user_console_set_diagnostics(0);
    size_t before=strlen(logtext);user_console_diagnostic("hidden",1);
    if(strlen(logtext)!=before)return 1;
    puts("input diagnostics: default silent, explicit enable, stage/results and disable PASS");
    return 0;
}

/* Actual production editor/reader: ownership, boundaries and completed-line delivery. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "input_console.h"
#include "keyboard.h"

static char output[8192];
static unsigned used, erased, sleeps, event_at, event_count;
static const char *events;
static int cancellation_checks;
static int cancel_after_wait(void *ctx) { assert(ctx==&cancellation_checks);return ++cancellation_checks==2; }
void serial_putc(char c) { assert(used+1 < sizeof(output)); output[used++]=c; output[used]=0; }
void serial_puts(const char *s) { while (*s) serial_putc(*s++); }
void console_erase_previous(void) { erased++; }
char keyboard_poll(struct keyboard *k)
{
    assert(k->ready && event_at < event_count);
    return events[event_at++];
}
void thread_sleep(uint64_t ticks) { assert(ticks==1); sleeps++; }
static void sequence(const char *s, unsigned n) { events=s; event_count=n; event_at=0; }
static void feed(struct input_console_line *line, const char *s)
{
    while (*s) input_console_accept(line, *s++);
}

int main(void)
{
    struct {
        uint32_t before;
        struct input_console_line line;
        uint32_t after;
    } guard = { .before=0x12345678, .after=0xABCDEF01 };
    struct input_console_line *line=&guard.line;
    char out[INPUT_CONSOLE_LINE_MAX+2], sentinel[sizeof(out)];
    memset(out, 0x5A, sizeof(out)); memcpy(sentinel, out, sizeof(out));
    assert(input_console_take_line(line, out, sizeof(out))==INPUT_CONSOLE_PENDING);
    assert(!memcmp(out,sentinel,sizeof(out)));
    feed(line,"\b\beditx\b!");
    assert(line->length==5 && !strcmp(line->bytes,"edit!") && erased==1);
    assert(input_console_take_line(line,out,sizeof(out))==INPUT_CONSOLE_PENDING);
    assert(!memcmp(out,sentinel,sizeof(out)));
    input_console_accept(line,'\n');
    struct input_console_line sealed=*line;
    unsigned output_at=used;
    feed(line,"ignored\b\n");
    assert(!memcmp(line,&sealed,sizeof(*line)) && used==output_at);
    assert(input_console_take_line(line,out,5)==INPUT_CONSOLE_NOSPACE); /* no room for NUL */
    assert(!memcmp(out,sentinel,sizeof(out)) && !memcmp(line,&sealed,sizeof(*line)));
    assert(input_console_take_line(line,NULL,sizeof(out))==INPUT_CONSOLE_NOSPACE);
    assert(input_console_take_line(line,out,6)==5 && !strcmp(out,"edit!"));
    assert(out[6]==0x5A && !line->ready && !line->length && !line->bytes[0]);
    feed(line,"new"); assert(!strcmp(out,"edit!")); /* caller owns the completed copy */
    *line=(struct input_console_line){0};
    feed(line,"\b\n"); assert(input_console_take_line(line,out,1)==0 && !out[0]);
    puts("PASS: completed-line ownership, Enter sealing, prompt boundary, empty lines and atomic capacity retry");

    for (int c=' '; c<='~'; c++) input_console_accept(line,(char)c);
    assert(line->length==95);
    unsigned count=line->length;
    input_console_accept(line,0x1B); input_console_accept(line,0x7F); input_console_accept(line,(char)0xFF);
    assert(line->length==count);
    *line=(struct input_console_line){0};
    for (unsigned i=0; i<INPUT_CONSOLE_LINE_MAX; i++) input_console_accept(line,'a');
    assert(line->length==128 && !line->overflow && !line->bytes[128]);
    input_console_accept(line,'\n');
    memset(out,0x5A,sizeof(out));
    assert(input_console_take_line(line,out,129)==128 && !out[128] && out[129]==0x5A);
    for (unsigned i=0; i<128; i++) assert(out[i]=='a');
    for (unsigned i=0; i<129; i++) input_console_accept(line,'b');
    assert(line->length==128 && line->overflow);
    feed(line,"\b\n"); /* editing never turns a truncated command into an executable one */
    memcpy(sentinel,out,sizeof(out));
    assert(input_console_take_line(line,out,sizeof(out))==INPUT_CONSOLE_TOOLONG);
    assert(!memcmp(out,sentinel,sizeof(out)) && !line->length && !line->overflow && !line->ready);
    assert(guard.before==0x12345678 && guard.after==0xABCDEF01);
    puts("PASS: printable filtering, exact 128-byte/NUL boundary, canaries and whole-line overflow discard");

    struct keyboard keyboard={.ready=1};
    unsigned before=used;
    assert(input_console_read_line(NULL,out,sizeof(out))==INPUT_CONSOLE_UNAVAILABLE);
    assert(input_console_read_line(&keyboard,out,128)==INPUT_CONSOLE_NOSPACE);
    assert(input_console_read_line(&keyboard,NULL,129)==INPUT_CONSOLE_NOSPACE && used==before);
    char stream[160];
    memset(stream,'x',129); stream[129]='\n';
    memcpy(stream+130,"\bworkx\b!\nnext\n\n",15);
    sequence(stream,145);
    used=0; output[0]=0;
    assert(input_console_read_line(&keyboard,out,sizeof(out))==5 && !strcmp(out,"work!"));
    assert(strstr(output,"input: line too long; discarded (128 characters maximum)\r\ninput> "));
    assert(input_console_read_line(&keyboard,out,sizeof(out))==4 && !strcmp(out,"next"));
    assert(input_console_read_line(&keyboard,out,sizeof(out))==0 && !out[0]);
    assert(event_at==145 && sleeps>0);
    memset(out,0x5A,sizeof(out));memcpy(sentinel,out,sizeof(out));sequence("",1);
    assert(input_console_read_line_for(&keyboard,out,sizeof(out),"",cancel_after_wait,&cancellation_checks)==INPUT_CONSOLE_INTERRUPTED);
    assert(cancellation_checks==2 && event_at==1 && !memcmp(out,sentinel,sizeof(out)));
    puts("PASS: blocking reader retries invalid lines, delivers edited sequential/empty lines and yields via PIT sleep");
    puts("PASS: cancellation after scheduler wait leaves destination unchanged");
    return 0;
}

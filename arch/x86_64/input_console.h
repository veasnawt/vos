#ifndef VOS_X86_64_INPUT_CONSOLE_H
#define VOS_X86_64_INPUT_CONSOLE_H

/* Opt-in exact boot token, shared with the focused host test. */
int input_console_requested(const char *cmdline);
#define INPUT_CONSOLE_LINE_MAX 128u
enum input_console_result {
    INPUT_CONSOLE_PENDING = -1,
    INPUT_CONSOLE_NOSPACE = -2,
    INPUT_CONSOLE_TOOLONG = -3,
    INPUT_CONSOLE_UNAVAILABLE = -4,
    INPUT_CONSOLE_INTERRUPTED = -5,
};
struct input_console_line {
    unsigned length;
    char bytes[INPUT_CONSOLE_LINE_MAX + 1];
    int ready, overflow;
};
/* Zero-initialize line before use. Append/delete at the end and echo accepted
 * input. Enter seals the line until take_line consumes it; no new prompt yet.
 * Any overflow makes this entire submission invalid, even after Backspace.
 */
void input_console_accept(struct input_console_line *line, char c);
/* Copy a completed, NUL-terminated line to caller-owned storage, returning its
 * length (0 is a valid empty line). Success clears the editor. PENDING/NOSPACE
 * change neither editor nor destination; TOOLONG discards the invalid line and
 * leaves destination untouched. This is a kernel C interface, not a user ABI.
 */
int input_console_take_line(struct input_console_line *line, char *out, unsigned capacity);
struct keyboard;
/* Blocking thread-context reader for a future shell/input service. Keyboard
 * must be initialized; out must have at least LINE_MAX+1 bytes. Retries after
 * an explicitly reported overlong line; returns only complete valid lines.
 */
int input_console_read_line(struct keyboard *keyboard, char *out, unsigned capacity);
/* Same reader for a granted process. Optional cancellation is checked after
 * every scheduler wait; prompt is trusted kernel text, not a user pointer. */
int input_console_read_line_for(struct keyboard *keyboard, char *out, unsigned capacity,
                                const char *prompt, int (*cancelled)(void *), void *ctx);
/* Buffered input demonstration; returns if PS/2 is unavailable. No shell yet. */
void input_console_run(void);

#endif

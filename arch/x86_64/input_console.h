#ifndef VOS_X86_64_INPUT_CONSOLE_H
#define VOS_X86_64_INPUT_CONSOLE_H

/* Opt-in exact boot token, shared with the focused host test. */
int input_console_requested(const char *cmdline);
struct input_console_line { unsigned length; };
/* Echo/edit one decoded character; never erase the prompt, cap at 128 cells. */
void input_console_accept(struct input_console_line *line, char c);
/* Echo console only; returns if the PS/2 controller is unavailable. No shell yet. */
void input_console_run(void);

#endif

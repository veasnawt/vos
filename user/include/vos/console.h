/* Additive ABI v3. No authority without an explicit trusted console grant. */
#ifndef VOS_USER_CONSOLE_H
#define VOS_USER_CONSOLE_H
#define VOS_SYS_CONSOLE_READ 11
#define VOS_SYS_CONSOLE_WRITE 12
#define VOS_SYS_CONSOLE_CLEAR 13
#define VOS_CONSOLE_LINE_MAX 128
#ifndef __ASSEMBLER__
long vos_console_read(void *destination, unsigned long capacity);
long vos_console_write(const void *source, unsigned long length);
long vos_console_clear(void);
#endif
#endif

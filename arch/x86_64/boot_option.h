#ifndef VOS_X86_64_BOOT_OPTION_H
#define VOS_X86_64_BOOT_OPTION_H
/* Exact whitespace-separated token; never accept a prefix or substring. */
static inline int boot_option(const char *p, const char *token)
{
    if (!p) return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        unsigned n = 0;
        while (token[n] && p[n] == token[n]) n++;
        if (!token[n] && (!p[n] || p[n] == ' ' || p[n] == '\t')) return 1;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}
#endif

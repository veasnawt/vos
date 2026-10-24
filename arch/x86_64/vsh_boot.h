#ifndef VOS_X86_64_VSH_BOOT_H
#define VOS_X86_64_VSH_BOOT_H
struct boot_info;
int vsh_requested(const char *cmdline);
int vsh_run(const struct boot_info *bi);
int rust_demo_requested(const char *cmdline);
int rust_demo_run(const struct boot_info *bi);
int rixie_requested(const char *cmdline);
int rixie_run(const struct boot_info *bi);
#endif

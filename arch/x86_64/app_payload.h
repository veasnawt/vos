#ifndef VOS_X86_64_APP_PAYLOAD_H
#define VOS_X86_64_APP_PAYLOAD_H
#include "app_catalog.h"
struct boot_info;
struct resv_table;
int app_payload_requested(const char *);
int app_payload_prepare(const struct boot_info *, const struct resv_table *);
const struct app_catalog *app_payload_catalog(void);
#endif

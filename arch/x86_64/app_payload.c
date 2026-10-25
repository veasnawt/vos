#include "app_payload.h"
#include "bootinfo.h"
#include "console.h"
#include "elf.h"
#include "lib.h"
#include "memlayout.h"
#include "resv.h"

static struct app_catalog catalog;
static const struct {
    uint64_t identity;
    const char *name;
    uint32_t policy;
} provisioned[] = {
    { 0x1001, "vsh", APP_POLICY_SHELL },
    { 0x1002, "hello", APP_POLICY_DIAGNOSTIC },
    { 0x1003, "rixie", APP_POLICY_CONSOLE }
};

int app_payload_requested(const char *p)
{
    static const char token[] = "vos.apps=1";
    if (!p) return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        unsigned i = 0;
        while (token[i] && p[i] == token[i]) i++;
        if (!token[i] && (!p[i] || p[i] == ' ' || p[i] == '\t')) return 1;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}

static int reject(void)
{
    memset(&catalog, 0, sizeof(catalog));
    serial_puts("catalog: rejected; no payload application may run\r\n");
    return 0;
}

int app_payload_prepare(const struct boot_info *bi, const struct resv_table *reserved)
{
    struct app_source sources[sizeof(provisioned) / sizeof(provisioned[0])] = { 0 };
    memset(&catalog, 0, sizeof(catalog));
    if (!bi || !reserved || reserved->count > RESV_MAX ||
        bi->module_count != sizeof(provisioned) / sizeof(provisioned[0]))
        return reject();
    unsigned seen = 0;
    for (unsigned i = 0; i < bi->module_count; i++) {
        const struct boot_module *m = &bi->modules[i];
        if (!m->cmdline || m->start >= m->end || m->end > BOOT_MAP_LIMIT ||
            m->end - m->start > ELF_MAX_FILE)
            return reject();
        unsigned n = 0;
        while (n <= APP_CATALOG_NAME_MAX && m->cmdline[n]) n++;
        if (!n || n > APP_CATALOG_NAME_MAX) return reject();
        unsigned match = sizeof(provisioned) / sizeof(provisioned[0]);
        for (unsigned j = 0; j < sizeof(provisioned) / sizeof(provisioned[0]); j++)
            if (strlen(provisioned[j].name) == n && !memcmp(m->cmdline, provisioned[j].name, n))
                match = j;
        if (match == sizeof(provisioned) / sizeof(provisioned[0]) || (seen & (1U << match)))
            return reject();
        /* Boot reservations never release pages. Validate coverage of every
         * source page before retaining its kernel-only alias; no copied code
         * or Multiboot ordinal becomes an application identity. */
        for (uint64_t p = m->start & ~(PAGE_SIZE - 1); p < m->end; p += PAGE_SIZE)
            if (!resv_overlaps(reserved, p, p + PAGE_SIZE)) return reject();
        struct app_source *s = &sources[match];
        s->version = APP_SOURCE_VERSION;
        s->identity = provisioned[match].identity;
        s->policy = provisioned[match].policy;
        memcpy(s->name, provisioned[match].name, n);
        s->image = phys_to_virt(m->start);
        s->length = m->end - m->start;
        seen |= 1U << match;
    }
    if (seen != (1U << (sizeof(provisioned) / sizeof(provisioned[0]))) - 1 ||
        app_catalog_build(&catalog, sources, sizeof(sources) / sizeof(sources[0])))
        return reject();
    serial_puts("catalog: ready "); serial_dec(catalog.count);
    serial_puts(" entries, "); serial_dec(sizeof(catalog));
    serial_puts(" bytes; all executable source pages reserved\r\n");
    for (unsigned i = 0; i < catalog.count; i++) {
        const struct app_catalog_entry *e = app_catalog_at(&catalog, i);
        serial_puts("catalog: "); serial_puts(e->name);
        serial_puts(" id="); serial_dec(e->identity);
        serial_puts(" ABI="); serial_dec(e->abi);
        serial_puts(" policy="); serial_dec(e->policy);
        serial_puts(" size="); serial_dec(e->length); serial_puts("\r\n");
    }
    return 1;
}

const struct app_catalog *app_payload_catalog(void)
{
    return &catalog;
}

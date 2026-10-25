#include "app_catalog.h"
#include "elf.h"
#include "lib.h"

static int name_valid(const char name[APP_CATALOG_NAME_MAX + 1])
{
    unsigned length = 0;
    while (length <= APP_CATALOG_NAME_MAX && name[length]) {
        unsigned char c = (unsigned char)name[length];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
        length++;
    }
    if (!length || length > APP_CATALOG_NAME_MAX)
        return 0;
    for (unsigned i = length; i <= APP_CATALOG_NAME_MAX; i++)
        if (name[i]) return 0;
    static const char *const commands[] = {
        "help", "clear", "echo", "pwd", "ls", "cd", "cat", "exit", "apps"
    };
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strlen(commands[i]) == length && !memcmp(commands[i], name, length))
            return 0;
    return 1;
}

int app_catalog_build(struct app_catalog *out, const struct app_source *sources, size_t count)
{
    if (!out) return APP_CATALOG_EINVAL;
    memset(out, 0, sizeof(*out));
    if (count > APP_CATALOG_MAX) return APP_CATALOG_ECAPACITY;
    if (!sources || !count) return APP_CATALOG_EINVAL;
    struct app_catalog candidate = { 0 };
    for (size_t i = 0; i < count; i++) {
        const struct app_source *s = &sources[i];
        if (s->version != APP_SOURCE_VERSION || !s->identity || !s->image ||
            !s->length || s->length > ELF_MAX_FILE ||
            (uintptr_t)s->image > UINTPTR_MAX - s->length)
            return APP_CATALOG_EINVAL;
        if (!name_valid(s->name)) return APP_CATALOG_ENAME;
        if (s->policy != APP_POLICY_SHELL && s->policy != APP_POLICY_DIAGNOSTIC &&
            s->policy != APP_POLICY_CONSOLE)
            return APP_CATALOG_EPOLICY;
        for (size_t j = 0; j < i; j++)
            if (s->identity == candidate.entries[j].identity ||
                !memcmp(s->name, candidate.entries[j].name, sizeof(s->name)))
                return APP_CATALOG_EDUPLICATE;
        struct elf_image image;
        if (elf_validate(s->image, s->length, &image) != ELF_OK)
            return APP_CATALOG_EEXEC;
        if (s->policy != APP_POLICY_DIAGNOSTIC && image.abi != 3)
            return APP_CATALOG_EPOLICY;
        struct app_catalog_entry *e = &candidate.entries[i];
        e->identity = s->identity;
        memcpy(e->name, s->name, sizeof(e->name));
        e->policy = s->policy;
        e->abi = image.abi;
        e->image = s->image;
        e->length = s->length;
    }
    candidate.count = (uint32_t)count;
    candidate.valid = 1;
    *out = candidate;
    return APP_CATALOG_OK;
}

const struct app_catalog_entry *app_catalog_at(const struct app_catalog *c, size_t index)
{
    return c && c->valid == 1 && c->count <= APP_CATALOG_MAX && index < c->count
        ? &c->entries[index] : NULL;
}

const struct app_catalog_entry *app_catalog_find(const struct app_catalog *c, uint64_t identity)
{
    if (!identity || !c || c->valid != 1 || c->count > APP_CATALOG_MAX) return NULL;
    for (size_t i = 0; i < c->count; i++)
        if (c->entries[i].identity == identity) return &c->entries[i];
    return NULL;
}

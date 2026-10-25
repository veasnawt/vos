/* Private kernel catalog; this is not an application ABI or an execution grant. */
#ifndef VOS_X86_64_APP_CATALOG_H
#define VOS_X86_64_APP_CATALOG_H
#include <stddef.h>
#include <stdint.h>

#define APP_CATALOG_MAX 8
#define APP_CATALOG_NAME_MAX 15
#define APP_SOURCE_VERSION 1
enum app_policy {
    APP_POLICY_SHELL = 1,
    APP_POLICY_DIAGNOSTIC = 2,
    APP_POLICY_CONSOLE = 3,
    APP_POLICY_GUI = 4
};
enum app_catalog_error {
    APP_CATALOG_OK = 0,
    APP_CATALOG_EINVAL = -1,
    APP_CATALOG_ECAPACITY = -2,
    APP_CATALOG_ENAME = -3,
    APP_CATALOG_EDUPLICATE = -4,
    APP_CATALOG_EPOLICY = -5,
    APP_CATALOG_EEXEC = -6
};
/* The caller owns readable, immutable source bytes for the catalog lifetime.
 * Name is zero-padded, lowercase ASCII [a-z0-9_-], with a terminating NUL.
 * Source metadata and executable buffers must be disjoint from the output.
 * These are kernel pointers, never user-supplied pointers or public identifiers. */
struct app_source {
    uint32_t version, policy;
    uint64_t identity;
    char name[APP_CATALOG_NAME_MAX + 1];
    const uint8_t *image;
    uint64_t length;
};
struct app_catalog_entry {
    uint64_t identity;
    char name[APP_CATALOG_NAME_MAX + 1];
    uint32_t policy, abi;
    const uint8_t *image;
    uint64_t length;
};
struct app_catalog {
    uint32_t count, valid;
    struct app_catalog_entry entries[APP_CATALOG_MAX];
};
/* No allocation or execution. Any invalid input clears the entire output.
 * A NULL output is rejected without access. Publication occurs only on success. */
int app_catalog_build(struct app_catalog *, const struct app_source *, size_t);
const struct app_catalog_entry *app_catalog_at(const struct app_catalog *, size_t);
const struct app_catalog_entry *app_catalog_find(const struct app_catalog *, uint64_t);
#endif

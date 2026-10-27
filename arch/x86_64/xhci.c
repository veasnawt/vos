/* USB.1 platform adapter only; all probe/validation decisions live in Rust. */
#include "xhci.h"

_Static_assert(sizeof(enum xhci_status) == 4 && sizeof(int) == 4 && sizeof(unsigned) == 4, "Rust scalar ABI");
_Static_assert(sizeof(struct xhci_info) == 104 && _Alignof(struct xhci_info) == 8, "Rust info ABI");
_Static_assert(offsetof(struct xhci_info, reason) == 8 && offsetof(struct xhci_info, vendor) == 16, "Rust info ABI");
_Static_assert(offsetof(struct xhci_info, class_revision) == 28 && offsetof(struct xhci_info, base) == 72, "Rust info ABI");
_Static_assert(offsetof(struct xhci_info, mapping_error) == 100, "Rust info ABI");
_Static_assert(sizeof(struct xhci_probe_ops) == 40 && offsetof(struct xhci_probe_ops, config) == 16, "Rust ops ABI");
_Static_assert(sizeof(struct pci_addr) == 3 && _Alignof(struct pci_addr) == 1, "Rust PCI ABI");
_Static_assert(XHCI_NOT_FOUND == 0 && XHCI_OK == 1 && XHCI_UNSUPPORTED == 2 && XHCI_INVALID == 3, "Rust status ABI");

/* Called synchronously by Rust with validated ops, seven writable words and
 * a distinct error integer. The existing map callback supplies a live UC view,
 * disjoint from Rust outputs/ops. No map lifetime/cache policy changes.
 * Return 0=copy, 1=map error, 2=invalid view. Exactly 28 bytes read; no writes.
 * The original public probe/decode/class symbols now live in the Rust archive. */
int vos_xhci_copy_caps(const struct xhci_probe_ops *ops, uint64_t base,
                       uint32_t out[7], int *error)
{
    const volatile uint32_t *mapped = 0;
    size_t available = 0;
    *error = ops->map(ops->context, base, XHCI_CAP_BYTES, &mapped, &available);
    if (*error) return 1;
    if (!mapped || ((uintptr_t)mapped & 3) || available < XHCI_CAP_BYTES) return 2;
    for (unsigned i = 0; i < XHCI_CAP_BYTES / 4; i++) out[i] = mapped[i];
    return 0;
}

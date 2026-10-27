#include "xhci.h"

static enum xhci_status outcome(struct xhci_info *r, enum xhci_status s, const char *why)
{
    r->status = s;
    r->reason = why;
    return s;
}

int xhci_class_matches(uint32_t revision)
{
    return (revision >> 8) == 0x0c0330;
}

static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

enum xhci_status xhci_decode_caps(const uint32_t *c, size_t bytes, struct xhci_info *r)
{
    if (!c || bytes < XHCI_CAP_BYTES)
        return outcome(r, XHCI_INVALID, "truncated capability header");
    r->caplength = c[0] & 0xff;
    r->version = c[0] >> 16;
    r->hcs1 = c[1]; r->hcs2 = c[2]; r->hcs3 = c[3]; r->hcc1 = c[4];
    r->slots = c[1] & 0xff;
    r->interrupters = (c[1] >> 8) & 0x7ff;
    r->ports = c[1] >> 24;
    r->doorbell = c[5]; r->runtime = c[6];
    if (c[0] == UINT32_MAX || (c[0] & 0xff00) ||
        r->caplength < 0x20 || (r->caplength & 3) ||
        !r->slots || !r->ports || !r->interrupters)
        return outcome(r, XHCI_INVALID, "invalid capability structure");
    if (r->version != 0x100 && r->version != 0x110 && r->version != 0x120)
        return outcome(r, XHCI_UNSUPPORTED, "unsupported xHCI interface version");
    /* Offsets are byte offsets with reserved low bits, not values to multiply. */
    if ((r->doorbell & 3) || (r->runtime & 31))
        return outcome(r, XHCI_INVALID, "misaligned register offsets");
    uint32_t op_end = r->caplength + 0x400 + r->ports * 16;
    uint32_t db_bytes = (r->slots + 1) * 4;
    uint32_t rt_bytes = 0x20 + r->interrupters * 0x20;
    if (r->doorbell < op_end || r->runtime < op_end ||
        r->doorbell > XHCI_LAYOUT_LIMIT - db_bytes ||
        r->runtime > XHCI_LAYOUT_LIMIT - rt_bytes ||
        overlap(r->doorbell, db_bytes, r->runtime, rt_bytes))
        return outcome(r, XHCI_INVALID, "invalid or unsupported register layout");
    r->layout_end = r->doorbell + db_bytes;
    if (r->layout_end < r->runtime + rt_bytes) r->layout_end = r->runtime + rt_bytes;
    return outcome(r, XHCI_OK, "capabilities inspected; controller unchanged");
}

enum xhci_status xhci_probe(struct pci_addr a, const struct xhci_probe_ops *ops, struct xhci_info *r)
{
    *r = (struct xhci_info){0};
    if (!ops || !ops->config || !ops->check || !ops->map ||
        ops->physical_bits < 32 || ops->physical_bits > 52)
        return outcome(r, XHCI_INVALID, "invalid probe interface");
    uint32_t id = ops->config(ops->context, a, 0);
    r->vendor = id; r->device = id >> 16;
    if (r->vendor == 0xffff)
        return outcome(r, XHCI_NOT_FOUND, "PCI function absent");
    r->class_revision = ops->config(ops->context, a, 8);
    if (!xhci_class_matches(r->class_revision))
        return outcome(r, XHCI_NOT_FOUND, "PCI class is not USB xHCI");
    if (!r->vendor || ((ops->config(ops->context, a, 12) >> 16) & 0x7f))
        return outcome(r, XHCI_INVALID, "invalid PCI endpoint header");
    uint32_t cs = ops->config(ops->context, a, PCI_COMMAND);
    r->command = cs; r->pci_status = cs >> 16;
    r->bar_low = ops->config(ops->context, a, 0x10);
    if (r->bar_low & 1)
        return outcome(r, XHCI_UNSUPPORTED, "I/O BAR unsupported");
    unsigned type = (r->bar_low >> 1) & 3;
    if (type == 1)
        return outcome(r, XHCI_UNSUPPORTED, "legacy below-1-MiB BAR unsupported");
    if (type == 3)
        return outcome(r, XHCI_INVALID, "reserved memory BAR type");
    r->address_bits = type == 2 ? 64 : 32;
    r->bar_high = type == 2 ? ops->config(ops->context, a, 0x14) : 0;
    r->base = ((uint64_t)r->bar_high << 32) | (r->bar_low & ~15u);
    if (!r->base)
        return outcome(r, XHCI_INVALID, "unassigned memory BAR");
    /* Conservative USB.1 support boundary; no destructive BAR sizing writes. */
    if (r->base & 0xfff)
        return outcome(r, XHCI_UNSUPPORTED, "BAR is not page aligned");
    if (r->base > UINT64_MAX - XHCI_CAP_BYTES)
        return outcome(r, XHCI_INVALID, "BAR address overflow");
    if (r->base + XHCI_CAP_BYTES > (1ULL << ops->physical_bits))
        return outcome(r, XHCI_UNSUPPORTED, "BAR exceeds supported physical width");
    if (!(r->command & PCI_COMMAND_MEMORY))
        return outcome(r, XHCI_UNSUPPORTED, "firmware left memory decoding disabled");
    r->mapping_error = ops->check(ops->context, r->base, XHCI_CAP_BYTES);
    if (r->mapping_error)
        return outcome(r, XHCI_UNSUPPORTED, "device-memory range rejected");
    const volatile uint32_t *mapped = 0;
    size_t available = 0;
    r->mapping_error = ops->map(ops->context, r->base, XHCI_CAP_BYTES, &mapped, &available);
    if (r->mapping_error)
        return outcome(r, XHCI_UNSUPPORTED, "device-memory mapping failed");
    if (!mapped || ((uintptr_t)mapped & 3) || available < XHCI_CAP_BYTES)
        return outcome(r, XHCI_INVALID, "invalid capability mapping");
    uint32_t caps[XHCI_CAP_BYTES / 4];
    for (unsigned i = 0; i < XHCI_CAP_BYTES / 4; i++) caps[i] = mapped[i];
    enum xhci_status s = xhci_decode_caps(caps, sizeof(caps), r);
    if (s != XHCI_OK) return s;
    if (r->base > UINT64_MAX - r->layout_end)
        return outcome(r, XHCI_INVALID, "register address overflow");
    if (r->base + r->layout_end > (1ULL << ops->physical_bits) ||
        (r->address_bits == 32 && r->base + r->layout_end > (1ULL << 32)))
        return outcome(r, XHCI_UNSUPPORTED, "register layout exceeds address width");
    /* The declared offsets are validated, never dereferenced in USB.1.
     * Mapping is limited to the fixed capability header; BAR aperture unknown. */
    return s;
}

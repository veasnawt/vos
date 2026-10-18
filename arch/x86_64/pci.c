#include "pci.h"

#define CONFIG_ADDRESS  0xCF8
#define CONFIG_DATA     0xCFC

static inline void out32(uint16_t port, uint32_t v)
{
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port) : "memory");
}

static inline uint32_t in32(uint16_t port)
{
    uint32_t v;

    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static inline void out16(uint16_t port, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port) : "memory");
}

static inline uint16_t in16(uint16_t port)
{
    uint16_t v;

    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static inline void out8(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port) : "memory");
}

static inline uint8_t in8(uint16_t port)
{
    uint8_t v;

    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

/* Select the dword holding off; the kernel runs with IF = 0, so nothing interleaves. */
static void cfg_select(struct pci_addr a, unsigned off)
{
    out32(CONFIG_ADDRESS, 0x80000000u | (uint32_t)a.bus << 16 | (uint32_t)(a.dev & 31) << 11 |
                          (uint32_t)(a.fn & 7) << 8 | (off & 0xFC));
}

uint8_t pci_read8(struct pci_addr a, unsigned off)
{
    cfg_select(a, off);
    return in8(CONFIG_DATA + (off & 3));
}

uint16_t pci_read16(struct pci_addr a, unsigned off)
{
    cfg_select(a, off);
    return in16(CONFIG_DATA + (off & 2));
}

uint32_t pci_read32(struct pci_addr a, unsigned off)
{
    cfg_select(a, off);
    return in32(CONFIG_DATA);
}

void pci_write8(struct pci_addr a, unsigned off, uint8_t v)
{
    cfg_select(a, off);
    out8(CONFIG_DATA + (off & 3), v);
}

void pci_write16(struct pci_addr a, unsigned off, uint16_t v)
{
    cfg_select(a, off);
    out16(CONFIG_DATA + (off & 2), v);
}

void pci_write32(struct pci_addr a, unsigned off, uint32_t v)
{
    cfg_select(a, off);
    out32(CONFIG_DATA, v);
}

int pci_scan_bus0(int (*match)(struct pci_addr a, void *ctx), void *ctx, struct pci_addr *found)
{
    for (unsigned d = 0; d < PCI_SCAN_DEVICES; d++) {
        struct pci_addr a = { 0, (uint8_t)d, 0 };
        unsigned fns = 1;

        if (pci_read16(a, PCI_VENDOR_ID) == 0xFFFF)
            continue;
        if (pci_read8(a, PCI_HEADER_TYPE) & 0x80)        /* multi-function device */
            fns = PCI_SCAN_FUNCTIONS;
        for (unsigned f = 0; f < fns; f++) {
            a.fn = (uint8_t)f;
            if (pci_read16(a, PCI_VENDOR_ID) == 0xFFFF)
                continue;
            if (match(a, ctx)) {
                *found = a;
                return 1;
            }
        }
    }
    return 0;
}

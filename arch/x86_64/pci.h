/*
 * Minimal PCI configuration access (N3.1): configuration mechanism #1
 * (ports 0xCF8/0xCFC) and a bounded scan of bus 0, only what the virtio-blk
 * driver needs. There is no PCI framework, no bridge traversal, no BAR
 * assignment and no MMIO mapping: QEMU's firmware has configured the
 * devices, and the virtio driver reaches its registers through PCI
 * configuration space (virtio_blk.c).
 */
#ifndef VOS_X86_64_PCI_H
#define VOS_X86_64_PCI_H

#include <stdint.h>

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION        0x08
#define PCI_HEADER_TYPE     0x0E
#define PCI_SUBSYSTEM_ID    0x2E
#define PCI_CAP_PTR         0x34

#define PCI_COMMAND_IO          (1u << 0)
#define PCI_COMMAND_MEMORY      (1u << 1)
#define PCI_COMMAND_MASTER      (1u << 2)
#define PCI_COMMAND_INTX_OFF    (1u << 10)
#define PCI_STATUS_CAP_LIST     (1u << 4)

/* Bus 0 only: 32 devices x 8 functions. */
#define PCI_SCAN_DEVICES    32
#define PCI_SCAN_FUNCTIONS  8

struct pci_addr {
    uint8_t bus, dev, fn;
};

/* Configuration space reads and writes; off must be naturally aligned and < 256. */
uint8_t pci_read8(struct pci_addr a, unsigned off);
uint16_t pci_read16(struct pci_addr a, unsigned off);
uint32_t pci_read32(struct pci_addr a, unsigned off);
void pci_write8(struct pci_addr a, unsigned off, uint8_t v);
void pci_write16(struct pci_addr a, unsigned off, uint16_t v);
void pci_write32(struct pci_addr a, unsigned off, uint32_t v);

/*
 * Visit every present function on bus 0 (at most 256 configuration reads of
 * the vendor ID plus one header-type read per device) until match returns
 * nonzero; then *found is that function and the result is 1. 0 if none.
 */
int pci_scan_bus0(int (*match)(struct pci_addr a, void *ctx), void *ctx, struct pci_addr *found);

#endif

/* USB.2-C2: kernel PMM frames for the Rust DMA ownership pool, and the
 * opt-in software-only exercise (vos.usb2c2-dma=1). These are ordinary CPU
 * RAM frames: no device address is produced and nothing is published to any
 * device. No PCI or xHCI register is accessed. Not an application interface. */
#ifndef VOS_X86_64_USB_DMA_H
#define VOS_X86_64_USB_DMA_H
#include <stdint.h>

#define USB_DMA_FRAMES 8u   /* allow-list bound: Rust pool records */

/* Kernel CR3, IF=0, non-IRQ thread context. 0, or a negative USB_DMA_E*. */
#define USB_DMA_EINVAL  (-1)
#define USB_DMA_ESTATE  (-2)
#define USB_DMA_ENOMEM  (-3)
#define USB_DMA_EFAULT  (-4)    /* injected failure (exercise only) */
int usb_dma_frame_alloc(uint64_t *frame);
int usb_dma_frame_zero(uint64_t frame);
int usb_dma_frame_free(uint64_t frame);
/* Fail the n-th following call of each operation (-1: never). */
void usb_dma_faults(int alloc_at, int zero_at, int free_at);
unsigned usb_dma_frames_held(void);

/* Filled by vos_usb_dma_exercise_v1 (Rust); layout asserted on both sides. */
struct usb_dma_report {
    uint64_t set_frames[4], retained_frame;
    int32_t status;             /* 0 pass, otherwise the failing stage */
    uint32_t allocated, prepared, released, quarantined, active_now, retained_now;
    uint32_t allocation_failures, rollback_failures, violations;
    uint32_t budget_refused, nondma_refused, wrong_state_refused, injected_steps, retained_state;
};
int32_t vos_usb_dma_exercise_v1(struct usb_dma_report *out);
/* 1 if not requested or passed; 0 on failure (boot check failure). */
int usb_dma_boot(const char *cmdline);
#endif

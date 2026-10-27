/* USB.2-B private kernel, non-DMA/non-device fixture authority. No syscall. */
#ifndef VOS_USB_RESOURCES_H
#define VOS_USB_RESOURCES_H
#include <stdint.h>

#define USB_RESOURCE_CONTROL 1u
#define USB_RESOURCE_MAPPING 2u
struct usb_resource { uint64_t token, address; };
int usb_resource_acquire(unsigned kind, struct usb_resource *out);
int usb_resource_release(unsigned kind, uint64_t token);
void usb_resource_retain(unsigned kind, uint64_t token);
int usb_resources_verify(const char *cmdline);
/* The Rust entry exercises real private C services, not model counters. */
int vos_rust_resource_cycle(void);
int vos_rust_resource_abandon(void);
#endif

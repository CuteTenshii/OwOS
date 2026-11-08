/* virtiogpu.h: virtio-gpu 2D display adapter (PCI 1af4:1050, QEMU `-vga virtio` / `virtio-gpu-pci`).
 *
 * Creates one host resource backed by guest RAM, scans it out, and registers a present hook that
 * transfers and flushes the damaged rectangle. The RAM is handed to fb_adopt as the framebuffer.
 */
#ifndef OWOS_VIRTIOGPU_H
#define OWOS_VIRTIOGPU_H

#include <stdint.h>

/* Bring the adapter up at want_w x want_h, 32-bpp, and adopt it as the system framebuffer.
 * Returns 0 on success, -1 if no virtio-gpu device is present or setup fails. */
int virtiogpu_init(uint32_t want_w, uint32_t want_h);

#endif /* OWOS_VIRTIOGPU_H */

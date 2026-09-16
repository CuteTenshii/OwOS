/* vmsvga.h — VMware SVGA II paravirtual display adapter (PCI 15ad:0405).
 *
 * VMware's GPU (QEMU's `-vga vmware`). Unlike the Bochs DISPI adapter fb.c
 * programs, it isn't VBE-compatible, so under VMware/`-vga vmware` neither
 * fb_adopt (the bootloader hands us no framebuffer on the QEMU `-kernel` path)
 * nor fb_init finds anything. This driver sets a 32-bpp linear mode, then hands
 * the framebuffer to fb_adopt and registers a present hook so the compositor's
 * damaged rectangles are flushed to the host through the device's command FIFO.
 */
#ifndef OWOS_VMSVGA_H
#define OWOS_VMSVGA_H

#include <stdint.h>

/* Bring the adapter up at (up to) want_w x want_h, 32-bpp, and adopt it as the
 * system framebuffer. Returns 0 on success (graphics mode live), -1 if no SVGA
 * II device is present — caller then falls through to fb_init. */
int vmsvga_init(uint32_t want_w, uint32_t want_h);

#endif /* OWOS_VMSVGA_H */

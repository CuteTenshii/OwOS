/* vdagent.h: QEMU host<->guest clipboard over virtio-serial (spice vdagent protocol).
 *
 * QEMU side: `-chardev qemu-vdagent,id=vdagent,clipboard=on -device virtio-serial-pci
 * -device virtserialport,chardev=vdagent,name=com.redhat.spice.0,nr=1`. Polled; the exchange only
 * advances when vdagent_clip_recv / vdagent_clip_send run (vmclipd calls them through /proc/hostclip). */
#ifndef OWOS_VDAGENT_H
#define OWOS_VDAGENT_H

/* Find the virtio-serial device and start the port handshake. No-op and silent if absent. */
void vdagent_init(void);

/* 1 once vdagent_init found a usable virtio-serial device. */
int vdagent_present(void);

/* Copy the newest host clipboard text into buf (at most max bytes). Returns its length, 0 if nothing new. */
int vdagent_clip_recv(char *buf, int max);

/* Offer len bytes of guest text to the host clipboard. Returns len, or -1 if the channel is down. */
int vdagent_clip_send(const char *buf, int len);

#endif /* OWOS_VDAGENT_H */

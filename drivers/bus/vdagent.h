/* vdagent.h: QEMU host<->guest clipboard over virtio-serial (spice vdagent protocol).
 *
 * QEMU side: `-chardev qemu-vdagent,id=vdagent,clipboard=on -device virtio-serial-pci
 * -device virtserialport,chardev=vdagent,name=com.redhat.spice.0,nr=1`. Runs on the virtioserial poll
 * thread; vmclipd reaches it through /proc/hostclip. */
#ifndef OWOS_VDAGENT_H
#define OWOS_VDAGENT_H

/* 1 once the host announced the vdagent port. */
int vdagent_present(void);

/* Copy the newest host clipboard text into buf (at most max bytes). Returns its length, 0 if nothing new. */
int vdagent_clip_recv(char *buf, int max);

/* Offer len bytes of guest text to the host clipboard. Returns len, or -1 if the channel is down. */
int vdagent_clip_send(const char *buf, int len);

#endif /* OWOS_VDAGENT_H */

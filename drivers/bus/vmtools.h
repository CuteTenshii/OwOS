/* vmtools.h — a minimal VMware Tools guest agent over the backdoor.
 *
 * Host-integration features that ride the backdoor (no virtio, no daemon
 * protocol of their own): clock sync from the host, a `guestinfo` publish so the
 * VM's variables are readable from the host, a periodic heartbeat so the
 * hypervisor sees the guest as alive, and a shared clipboard synced with the
 * host (copy/paste from/to the VM). Real VMware only — QEMU's vmport answers
 * the version probe but not the message channel, so on QEMU this detects the
 * backdoor, logs that the RPC channel is unavailable, and does nothing else. */
#ifndef OWOS_VMTOOLS_H
#define OWOS_VMTOOLS_H

/* Detect VMware, sync the clock from the host, publish guestinfo, and (if the
 * GuestRPC channel works) spawn a heartbeat thread. No-op and silent off VMware.
 * Call after the scheduler and timer are up (it may spawn a thread / sleep). */
void vmtools_init(void);

/* Shared clipboard, bridged to the host on VMware.
 *
 * vmtools_clip_set replaces the clipboard with len bytes of text and, on a
 * VMware guest, pushes it up to the host clipboard (guest->host copy). It always
 * keeps a local copy, so copy/paste also works as a plain in-OS clipboard off
 * VMware.
 *
 * vmtools_clip_get copies the current clipboard into out (at most max bytes) and
 * returns the byte count. On VMware the local copy is kept fresh by a poll
 * thread watching the host clipboard, so this yields host-copied text too
 * (host->guest paste). Safe to call from interrupt context — a plain memory
 * copy, no backdoor I/O. */
void vmtools_clip_set(const char *s, int len);
int  vmtools_clip_get(char *out, int max);

#endif /* OWOS_VMTOOLS_H */

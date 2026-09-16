/* vmtools.h — a minimal VMware Tools guest agent over the backdoor.
 *
 * Host-integration features that ride the backdoor (no virtio, no daemon
 * protocol of their own): clock sync from the host, a `guestinfo` publish so
 * the VM's variables are readable from the host, a periodic heartbeat so the
 * hypervisor sees the guest as alive. Real VMware only — QEMU's vmport answers
 * the version probe but not the message channel, so on QEMU this detects the
 * backdoor, logs that the RPC channel is unavailable, and does nothing else.
 *
 * Host<->guest clipboard sync also rides this backdoor, but the bridge itself
 * is user space now (/bin/vmclipd, over the /proc/hostclip device node and
 * clipd) — the kernel no longer speaks to the clipboard server. */
#ifndef OWOS_VMTOOLS_H
#define OWOS_VMTOOLS_H

/* Detect VMware, sync the clock from the host, publish guestinfo, and (if the
 * GuestRPC channel works) spawn a heartbeat thread. No-op and silent off
 * VMware. Call after the scheduler and timer are up (it may spawn a thread /
 * sleep). */
void vmtools_init(void);

#endif /* OWOS_VMTOOLS_H */

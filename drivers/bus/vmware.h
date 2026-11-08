/* vmware.h — VMware hypervisor backdoor I/O channel.
 *
 * VMware (and QEMU's `vmport`) expose a magic I/O port that the guest pokes
 * with a special `in` instruction: the hypervisor reads the general-purpose
 * registers, services the command in ECX, and writes the results back into the
 * same registers. It's the transport under vmmouse, time sync, the balloon, and
 * the guest RPC channel. We only use the low-bandwidth single-instruction form.
 */
#ifndef OWOS_VMWARE_H
#define OWOS_VMWARE_H

#include <stdint.h>

#define VMWARE_MAGIC 0x564D5868u /* "VMXh" — EAX must hold this on every call  */
#define VMWARE_PORT 0x5658       /* the backdoor I/O port (EDX)                 */

#define VMWARE_CMD_GETVERSION 10

/* The register file shuffled through the backdoor. EAX is the magic on the way
 * in and a status/result on the way out; EBX/ECX/EDX carry command parameters
 * and results; ESI/EDI are used only by the bulk RPC commands we don't issue.
 */
struct vmware_regs {
  uint32_t eax, ebx, ecx, edx, esi, edi;
};

/* Raw backdoor call: execute the magic `in` with r preloaded; r is overwritten
 * with the hypervisor's reply. Callers normally use vmware_send instead. */
void vmware_backdoor(struct vmware_regs *r);

/* Issue command `cmd`: fills EAX=magic, ECX=cmd, EDX=port; the caller preloads
 * r->ebx (the parameter) and reads the reply out of r afterwards. */
void vmware_send(uint16_t cmd, struct vmware_regs *r);

/* 1 if a VMware-compatible backdoor answered GETVERSION (VMware, or QEMU with
 * vmport on). Probed once and cached; safe to call on bare metal — a machine
 * with no backdoor leaves EBX unchanged, so it reads as absent. */
int vmware_present(void);

/* Send a GuestRPC request string over the backdoor message channel — the
 * transport VMware Tools uses for guestinfo / heartbeat / host queries. Up to
 * resp_max-1 bytes of the reply are written NUL-terminated into resp (the RPCI
 * reply's leading "1 "/"0 " status marker stripped). Returns the reply length
 * (>=0) on a success ("1 ") reply, -1 on a failure reply or any channel error.
 *
 * Real VMware only: QEMU's vmport implements GETVERSION + vmmouse but NOT the
 * message channel, so this returns -1 under QEMU. Pass NULL/0 for resp when the
 * reply doesn't matter (e.g. an info-set). */
int vmware_rpc(const char *req, char *resp, int resp_max);

/* Host-to-guest request channel ("TCLO"), polled. Each call ships the reply set by the previous
 * vmware_tclo_reply (if any), then fetches the next pending host request into req (NUL-terminated, at most max-1
 * bytes). Returns the request length, 0 if none is pending, -1 on a channel error (it reopens on the next call).
 * Real VMware only. */
int vmware_tclo_poll(char *req, int max);
void vmware_tclo_reply(const char *reply);

/* Legacy VMware copy/paste — the simple text-only clipboard channel that
 * predates GuestRPC (backdoor commands 6-9, four bytes per call). It rides the
 * plain single-instruction backdoor, so unlike vmware_rpc it does NOT need the
 * message channel.
 *
 * vmware_clip_recv pulls the host clipboard into buf (at most max bytes, the
 * rest drained and discarded so the host stays in sync); it returns the number
 * of bytes the host offered (capped to max) or -1 when the host has nothing new
 * to hand over. vmware_clip_send pushes len bytes of guest text up to the host
 * clipboard; it returns len, or -1 off VMware. Real VMware only — QEMU's vmport
 * does not implement these commands. */
int vmware_clip_recv(char *buf, int max);
int vmware_clip_send(const char *buf, int len);

#endif /* OWOS_VMWARE_H */

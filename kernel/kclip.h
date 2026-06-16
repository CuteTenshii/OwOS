/* kclip.h — kernel-side client of the clipboard server (/bin/clipd).
 *
 * The kernel sometimes needs the clipboard without a user process in the loop:
 * Ctrl+V injects the current clip as keystrokes, and (later) the VMware host
 * bridge mirrors text to/from the guest. Both go through clipd like everyone
 * else — the kernel just speaks the same protocol over an ipc_connect() channel.
 * Must be called from a thread context (it blocks on the channel), never an IRQ.
 */
#ifndef OWOS_KCLIP_H
#define OWOS_KCLIP_H

/* Copy the newest clip into out[max] when it is text; returns its length (capped
 * to max), or 0 if it isn't text, the clipboard is empty, or clipd is down. */
int kclip_get_text(char *out, int max);

/* Push `text` (len bytes) as a text/plain clip. Returns 0, or -1 if clipd is
 * unreachable. */
int kclip_set_text(const char *text, int len);

#endif /* OWOS_KCLIP_H */

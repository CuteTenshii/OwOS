/* log.h — the kernel journal: a separate, always-on boot/diagnostic log.
 *
 * During bring-up the console doubles as the kernel log. Every character written
 * to it is recorded here (see vga.c) so the verbose subsystem log survives into
 * a ring buffer the `journal` command replays. Whether the log is *shown on
 * screen* as it happens is gated by the `verbose` boot argument; the journal
 * records either way, and the serial log port always streams it. Once the shell
 * owns the console, capture stops -- so the journal stays the boot log and the
 * terminal is a separate surface, not a mirror of it. */
#ifndef OWOS_LOG_H
#define OWOS_LOG_H

/* Append one character to the journal ring buffer. The console funnel
 * (con_putc) calls this while capture is on. */
void journal_record(char c);

/* Kernel log line / unsigned decimal. Always recorded to the journal; drawn to
 * the console only when verbose. Use these (not con_write) for diagnostics that
 * may fire after the shell owns the screen -- async events like a DHCP lease
 * completing -- so they land in the journal, not in the middle of the user's
 * terminal. Synchronous boot logging can keep using plain con_write: the console
 * funnel journals and gates it automatically while capture is on. */
void klog(const char *s);
void klog_dec(unsigned int value);

/* Boot-phase capture switch. On from boot; the shell turns it off when it takes
 * over the console, so interactive traffic isn't logged into the journal. */
void journal_capture(int on);
int  journal_capturing(void);

/* Replay the recorded journal to the console (the `journal` command). */
void journal_dump(void);

/* Replay the journal onto the framebuffer boot console (catch the screen up to
 * the lines logged before graphics mode existed). Called once, after fb is up. */
void journal_to_fb(void);

/* Verbose policy. When set -- by a `verbose` token on the kernel command line --
 * the boot log is also drawn to the screen as it happens; otherwise the boot is
 * quiet and the log lives only in the journal (and on the serial log port). */
void log_set_verbose(int on);
int  log_verbose(void);

#endif /* OWOS_LOG_H */

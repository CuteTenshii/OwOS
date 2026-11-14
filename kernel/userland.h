/* userland.h: what every boot path does once the kernel is up: lay out the root filesystem, install the programs the
 * kernel carries, start init, and feed the recovery console. */
#ifndef OWOS_USERLAND_H
#define OWOS_USERLAND_H

/* The directory tree, the embedded payload, and the seed files a fresh system needs; files that already exist are
 * kept. `inittab` is init's service table (one /bin daemon per line) for a system that has none yet. */
void userland_prepare(const char *inittab);

/* Run /bin/init as PID 1, as root. */
void userland_start(void);

/* Hand one character, or a Ctrl+C, to the recovery console, under the kernel lock. */
void console_feed(char c);
void console_interrupt(void);

#endif /* OWOS_USERLAND_H */

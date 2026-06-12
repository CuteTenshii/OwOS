/* screenshot.h — Print Screen capture of the whole framebuffer to a PNG.
 *
 * The keyboard IRQ can't do the heavy lifting (it would kmalloc megabytes and
 * block on disk), so it only *requests* a shot; the WM thread polls and runs
 * the actual capture in thread context. The image is written to the user's home
 * directory as /home/root/screenshotN.png.
 */
#ifndef OWOS_SCREENSHOT_H
#define OWOS_SCREENSHOT_H

/* Start the screenshot worker thread. Call once at boot, after the scheduler
 * is up. */
void screenshot_init(void);

/* Request a screenshot; the worker thread takes it. Safe to call from an IRQ
 * (Print Screen does). */
void screenshot_request(void);

#endif /* OWOS_SCREENSHOT_H */

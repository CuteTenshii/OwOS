/* edid.h — best-effort monitor EDID reader (real-hardware-only).
 *
 * Recovers the attached monitor's native refresh rate by reading its EDID over
 * DDC2/I2C, for the neofetch splash. The only transport implemented is Intel
 * GMBUS (the I2C controller in 0x8086 integrated graphics), which reaches the
 * DDC pins of an external HDMI/DVI/VGA monitor. Two things it deliberately does
 * NOT handle, because each is a separate per-GPU effort:
 *   - non-Intel GPUs (AMD/NVIDIA have their own I2C/GPIO blocks), and
 *   - an internal laptop eDP panel, whose EDID rides the DisplayPort AUX
 *     channel rather than I2C.
 * QEMU's emulated VGA exposes no EDID over GMBUS, so this is a no-op there
 * (edid_refresh_hz() returns 0). Like battery.c it is best-effort: it probes
 * once, fails gracefully, and records edid_error() for on-hardware debugging.
 */
#ifndef OWOS_EDID_H
#define OWOS_EDID_H

/* Native refresh rate (Hz) computed from the monitor's preferred detailed
 * timing, or 0 if no EDID could be read. Probes once on first call, caches. */
int edid_refresh_hz(void);

/* Preferred-mode width/height in pixels from the same descriptor (0 if
 * unknown). Shares the one-shot probe with edid_refresh_hz(). */
int edid_width(void);
int edid_height(void);

/* Why EDID couldn't be read (e.g. "no Intel GPU", "GMBUS NAK", "bad checksum"),
 * for real-hardware diagnostics. Empty string after a successful read. */
const char *edid_error(void);

#endif /* OWOS_EDID_H */

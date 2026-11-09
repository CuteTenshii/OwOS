/* edid.h — best-effort monitor EDID reader (real-hardware-only).
 *
 * Recovers the attached monitor's native refresh rate by reading its EDID, for
 * the neofetch splash. Two transports, tried in order, both Intel-only:
 *   - GMBUS (the I2C controller in 0x8086 integrated graphics), which reaches
 *     the DDC pins of an external HDMI/DVI/VGA monitor, and
 *   - the DDI DisplayPort AUX channels A-F (Gen9+ register layout), which carry
 *     EDID as I2C-over-AUX for external DP and internal eDP panels.
 * Non-Intel GPUs (AMD/NVIDIA have their own I2C/AUX blocks) are not handled.
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

/* Horizontal pixels per inch from the preferred timing and the panel's physical
 * width (0 if unknown). Shares the one-shot probe. */
int edid_dpi(void);

/* The attached monitor's brand+model decoded from EDID ("<brand> <model>", e.g.
 * "Dell U2412M"), written NUL-terminated into out; "" if no EDID was read. The
 * model half (the 0xFC monitor-name descriptor) is optional, so a bare panel
 * may yield brand only. Shares the one-shot probe. */
void edid_model(char *out, int len);

/* 1 if the display's PNP manufacturer id belongs to a known laptop-panel vendor
 * (AUO/BOE/LGD/...), the practical signal that it's an integrated screen — base
 * EDID has no explicit internal/external bit. */
int edid_internal(void);

/* 1 if the sink's EDID carries an HDMI vendor block; edid_hdmi_audio() also needs the CTA basic-audio flag. Both
 * share the one-shot probe and are 0 for a DVI/VGA/DP sink or with no EDID. */
int edid_hdmi(void);
int edid_hdmi_audio(void);

/* 1 if a monitor's EDID was read by the last probe (hotplug detect). edid_rescan() re-probes now, so a monitor plugged
 * in or pulled since the last probe shows up in every accessor above. */
int edid_connected(void);
void edid_rescan(void);

/* Why EDID couldn't be read (e.g. "no Intel GPU", "GMBUS NAK", "bad checksum"),
 * for real-hardware diagnostics. Empty string after a successful read. */
const char *edid_error(void);

#endif /* OWOS_EDID_H */

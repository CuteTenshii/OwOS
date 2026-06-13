/* mtrr.h — force a physical range (the framebuffer) to write-combining.
 *
 * fb.c asks for WC through PAT, but the effective memory type is the
 * combination of the MTRR type and the PAT type, and the more-uncached of the
 * two wins. On real UEFI hardware the firmware's MTRRs commonly leave the GOP
 * framebuffer uncached, silently overriding PAT-WC. mtrr_set_wc programs a
 * variable-range MTRR so the range is WC regardless of PAT.
 */
#ifndef OWOS_MTRR_H
#define OWOS_MTRR_H

#include <stdint.h>

/* Mark [phys, phys+len) write-combining via a variable MTRR, and dump the
 * current MTRR/PAT state plus the framebuffer's effective memory type to COM1
 * for on-hardware diagnosis. No-op (just the dump) when the CPU lacks MTRRs or
 * WC-type support, or when the range can't be expressed as one aligned MTRR.
 * Returns 0 if a WC MTRR was installed, -1 otherwise. */
int mtrr_set_wc(uint32_t phys, uint32_t len);

#endif /* OWOS_MTRR_H */

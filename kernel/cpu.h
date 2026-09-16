/* cpu.h — identify the processor via the CPUID instruction. */
#ifndef OWOS_CPU_H
#define OWOS_CPU_H

#include <stdint.h>

void cpu_vendor(char *buf13); /* 12-char vendor string + NUL ("GenuineIntel") */
void cpu_brand(char *buf49);  /* up to 48-char brand string + NUL            */

/* Family/model/stepping, decoded from CPUID leaf 1 EAX with the extended-field
 * fixups (so modern CPUs report a sane family/model, not 6/15). */
struct cpu_signature {
  uint32_t family;
  uint32_t model;
  uint32_t stepping;
};
void cpu_signature(struct cpu_signature *sig);

/* Physical/virtual address sizes in bits (CPUID leaf 0x80000008). Falls back to
 * the architectural 32/32 when the leaf is unavailable. */
void cpu_address_bits(uint32_t *phys, uint32_t *virt);

/* Feature flags as a NUL-terminated, space-separated lowercase list
 * ("fpu tsc pae apic sse sse2 ..."), written into buf (size bytes). The decoded
 * set covers leaf 1 EDX/ECX and leaf 0x80000001 EDX/ECX. */
void cpu_features(char *buf, uint32_t size);

/* Approximate TSC frequency in MHz, calibrated by counting cycles over a short
 * busy-wait window measured with the PIT. Returns 0 when the CPU has no TSC.
 * Must run with interrupts enabled (it relies on the timer advancing). */
uint32_t cpu_mhz(void);

#endif /* OWOS_CPU_H */

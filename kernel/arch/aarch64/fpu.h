/* fpu.h: per-task FP/SIMD state on AArch64: v0..v31 and the FPSR/FPCR control registers. The kernel is built
 * -mgeneral-regs-only, so it never touches them itself. */
#ifndef OWOS_FPU_H
#define OWOS_FPU_H

#include <stdint.h>

#define FPU_AREA_BYTES (32 * 16 + 16) /* q0..q31, then FPSR and FPCR as two doublewords */
#define FPU_AREA_ALIGN 16

/* Let EL0 and EL1 use FP/SIMD without trapping, and reset the registers. The first call records the clean state. */
void fpu_cpu_init(void);

void fpu_save(void *area);
void fpu_restore(const void *area);
void fpu_load_init(void);
/* Make a user-supplied image safe to restore: only FPCR's defined control bits survive. */
void fpu_sanitize(void *area);

#endif /* OWOS_FPU_H */

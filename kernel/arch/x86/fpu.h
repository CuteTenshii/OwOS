/* fpu.h: per-task floating-point state. i386 keeps the x87 file (fnsave); x86_64 keeps x87 and SSE (fxsave),
 * since its user ABI passes floats in xmm registers. The kernel itself never touches either. */
#ifndef OWOS_FPU_H
#define OWOS_FPU_H

#include <stdint.h>

#ifdef __x86_64__
#define FPU_AREA_BYTES 512
#else
#define FPU_AREA_BYTES 108
#endif
#define FPU_AREA_ALIGN 16

/* Enable the FPU (and SSE on x86_64) on the calling CPU and reset its state. The first call records that state as
 * the image every new task starts from. */
void fpu_cpu_init(void);

/* Save the calling CPU's FPU state to `area` (FPU_AREA_ALIGN aligned). On i386 this also resets the FPU. */
void fpu_save(void *area);
/* Load a state saved by fpu_save, or by fpu_sanitize after a user edit. */
void fpu_restore(const void *area);
/* Load the clean state new tasks start from. */
void fpu_load_init(void);
/* Make a user-supplied image safe to restore: fxrstor faults in ring 0 on reserved MXCSR bits. */
void fpu_sanitize(void *area);

#endif /* OWOS_FPU_H */

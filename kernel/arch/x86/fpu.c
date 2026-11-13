/* fpu.c: per-task floating-point state (see fpu.h). */
#include "fpu.h"
#include "creg.h"
#include <stdbool.h>

#define CR0_MP (1u << 1)
#define CR0_EM (1u << 2)
#define CR0_TS (1u << 3)
#define CR0_NE (1u << 5) /* x87 errors raise #MF instead of the legacy IRQ 13 */

#ifdef __x86_64__
#define CR4_OSFXSR (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define MXCSR_DEFAULT 0x1F80u /* every SSE exception masked, round to nearest */
#define FXSAVE_MXCSR 24
#define FXSAVE_MXCSR_MASK 28

static uint32_t mxcsr_mask = 0xFFBFu; /* the architectural default when the CPU reports none */
#endif

static uint8_t init_image[FPU_AREA_BYTES] __attribute__((aligned(FPU_AREA_ALIGN)));
static bool init_ready;

void fpu_cpu_init(void)
{
  write_cr0((read_cr0() & ~(uintptr_t)(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
#ifdef __x86_64__
  write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
#endif
  __asm__ volatile("fninit");
#ifdef __x86_64__
  uint32_t mxcsr = MXCSR_DEFAULT;
  __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
#endif
  if (init_ready)
    return;
  fpu_save(init_image);
#ifdef __x86_64__
  uint32_t mask = *(const uint32_t *)(init_image + FXSAVE_MXCSR_MASK);
  if (mask)
    mxcsr_mask = mask;
#endif
  init_ready = true;
}

#ifdef __x86_64__
void fpu_save(void *area) { __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory"); }
void fpu_restore(const void *area) { __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory"); }
void fpu_sanitize(void *area) { *(uint32_t *)((uint8_t *)area + FXSAVE_MXCSR) &= mxcsr_mask; }
#else
void fpu_save(void *area) { __asm__ volatile("fnsave (%0)" : : "r"(area) : "memory"); }
void fpu_restore(const void *area) { __asm__ volatile("frstor (%0)" : : "r"(area) : "memory"); }
/* frstor takes any image: a bad one only raises #MF later, in ring 3. */
void fpu_sanitize(void *area) { (void)area; }
#endif

void fpu_load_init(void) { fpu_restore(init_image); }

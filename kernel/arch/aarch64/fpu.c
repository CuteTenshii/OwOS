/* fpu.c: per-task FP/SIMD state (see fpu.h). The kernel is built -mgeneral-regs-only, which keeps the compiler off
 * these registers but still lets the assembler take them here. */
#include "fpu.h"
#include "arch.h"

#define CPACR_FPEN (3u << 20) /* no FP/SIMD traps at EL0 or EL1 */
#define FPSR_OFF 512
#define FPCR_OFF 520
#define FPSR_STATUS 0xF800009Full  /* NZCV QC, and the cumulative exception flags */
#define FPCR_CONTROL 0x07C09F00ull /* AHP DN FZ RMode, and the exception trap enables */

/* All zero: round to nearest, no traps, no flags, every register clear. */
static const uint8_t init_image[FPU_AREA_BYTES] __attribute__((aligned(FPU_AREA_ALIGN)));

void fpu_cpu_init(void)
{
  SYSREG_WRITE(cpacr_el1, SYSREG_READ(cpacr_el1) | CPACR_FPEN);
  __asm__ volatile("isb");
  fpu_load_init();
}

void fpu_save(void *area)
{
  __asm__ volatile("stp q0, q1, [%0, #0]\n\tstp q2, q3, [%0, #32]\n\tstp q4, q5, [%0, #64]\n\t"
                   "stp q6, q7, [%0, #96]\n\tstp q8, q9, [%0, #128]\n\tstp q10, q11, [%0, #160]\n\t"
                   "stp q12, q13, [%0, #192]\n\tstp q14, q15, [%0, #224]\n\tstp q16, q17, [%0, #256]\n\t"
                   "stp q18, q19, [%0, #288]\n\tstp q20, q21, [%0, #320]\n\tstp q22, q23, [%0, #352]\n\t"
                   "stp q24, q25, [%0, #384]\n\tstp q26, q27, [%0, #416]\n\tstp q28, q29, [%0, #448]\n\t"
                   "stp q30, q31, [%0, #480]\n\t"
                   "mrs x9, fpsr\n\tstr x9, [%0, #512]\n\tmrs x9, fpcr\n\tstr x9, [%0, #520]"
                   :
                   : "r"(area)
                   : "x9", "memory");
}

void fpu_restore(const void *area)
{
  __asm__ volatile("ldp q0, q1, [%0, #0]\n\tldp q2, q3, [%0, #32]\n\tldp q4, q5, [%0, #64]\n\t"
                   "ldp q6, q7, [%0, #96]\n\tldp q8, q9, [%0, #128]\n\tldp q10, q11, [%0, #160]\n\t"
                   "ldp q12, q13, [%0, #192]\n\tldp q14, q15, [%0, #224]\n\tldp q16, q17, [%0, #256]\n\t"
                   "ldp q18, q19, [%0, #288]\n\tldp q20, q21, [%0, #320]\n\tldp q22, q23, [%0, #352]\n\t"
                   "ldp q24, q25, [%0, #384]\n\tldp q26, q27, [%0, #416]\n\tldp q28, q29, [%0, #448]\n\t"
                   "ldp q30, q31, [%0, #480]\n\t"
                   "ldr x9, [%0, #512]\n\tmsr fpsr, x9\n\tldr x9, [%0, #520]\n\tmsr fpcr, x9"
                   :
                   : "r"(area)
                   : "x9", "memory");
}

void fpu_load_init(void) { fpu_restore(init_image); }

void fpu_sanitize(void *area)
{
  *(uint64_t *)((uint8_t *)area + FPSR_OFF) &= FPSR_STATUS;
  *(uint64_t *)((uint8_t *)area + FPCR_OFF) &= FPCR_CONTROL;
}

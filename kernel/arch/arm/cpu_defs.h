/* cpu_defs.h: constants shared by C and assembly. */
#ifndef OWOS_ARCH_ARM_CPU_DEFS_H
#define OWOS_ARCH_ARM_CPU_DEFS_H

#define BOOT_STACK_SIZE 16384

#define PSR_MODE_MASK 0x1f
#define PSR_MODE_SVC 0x13
#define PSR_MODE_HYP 0x1a
#define PSR_F (1 << 6)
#define PSR_I (1 << 7)
#define PSR_A (1 << 8)

/* Exception kinds handed to arch_exception, in vector table order. */
#define TRAP_RESET 0
#define TRAP_UNDEF 1
#define TRAP_SVC 2
#define TRAP_PABT 3
#define TRAP_DABT 4
#define TRAP_UNUSED 5
#define TRAP_IRQ 6
#define TRAP_FIQ 7

#endif

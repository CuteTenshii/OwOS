/* pwrap.h: MediaTek MT6765 PMIC wrapper (PWRAP) and the MT6357-family PMIC behind it. */
#ifndef OWOS_ARCH_AARCH64_PWRAP_H
#define OWOS_ARCH_AARCH64_PWRAP_H

#include <stdint.h>

/* Attach to a PWRAP the bootloader already initialised. Returns 0, or -1 if it is not initialised. */
int pwrap_init(uintptr_t base);

/* 16-bit PMIC register access. reg must be even. Return 0, or -1 on a bad register or a bus timeout. */
int pmic_read(uint16_t reg, uint16_t *val);
int pmic_write(uint16_t reg, uint16_t val);

/* Identify the PMIC through its read-test register: "mt6357", "mt6358", or NULL if neither answers. */
const char *pmic_probe(void);

#endif

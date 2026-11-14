/* uabi.h: the word size and user address layout that the kernel and user programs share. */
#ifndef OWOS_COMMON_UABI_H
#define OWOS_COMMON_UABI_H

/* One syscall argument, result or request field. x86 keeps 32-bit words, as every user address there lies below
 * 4 GiB; AArch64 passes full registers, as its user window lies above the 4 GiB the kernel identity-maps. */
#ifdef __aarch64__
typedef unsigned long sys_word;
#define UABI_SHIFT 0xC0000000ul /* the x86 layout below, moved up by 3 GiB */
#else
typedef unsigned int sys_word;
#define UABI_SHIFT 0u
#endif

/* The private window each process runs in: image and heap from USER_BASE, main stack at the top. */
#define USER_BASE (UABI_SHIFT + 0x40000000u)
#define USER_REGION_END (UABI_SHIFT + 0x80000000u)

/* Shared memory maps at the start of an 8 MiB slot: SHM_LOW_SLOTS below the compositor's framebuffer back buffer,
 * SHM_HIGH_SLOTS above it. A slot spans whole page tables, so unmapping one never touches a neighbour. */
#define SHM_SLOT_SIZE 0x00800000u /* >= 1920x1080x4 */
#define SHM_LOW_ADDR (UABI_SHIFT + 0xC0000000u)
#define SHM_LOW_SLOTS 32
#define COMPOSITOR_FB_ADDR (UABI_SHIFT + 0xD0000000u)
#define SHM_HIGH_ADDR (UABI_SHIFT + 0xE0000000u)
#define SHM_HIGH_SLOTS 64

#endif /* OWOS_COMMON_UABI_H */

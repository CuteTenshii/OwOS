/* entropy.h — kernel entropy pool.
 *
 * Accumulates interrupt-timing jitter (a fresh TSC reading mixed in at every
 * timer tick and packet arrival) and extracts CSPRNG-quality bytes by hashing
 * the pool with SHA-256 in a ratcheting construction. Backs the TCP ISN and the
 * user-space getrandom() syscall, so key material no longer depends on RDRAND
 * being present. */
#ifndef OWOS_ENTROPY_H
#define OWOS_ENTROPY_H

#include <stdint.h>

/* Seed the pool at boot so extraction before enough tick/packet stirring depends
 * on more than a single TSC read. Mixes in hardware RDRAND (when CPUID reports it)
 * plus a batch of TSC samples. Call once early in kernel_main, before any consumer
 * (TCP ISN, getrandom). */
void entropy_init(void);

/* Mix `sample` plus a fresh TSC reading into the pool. Cheap and lock-free —
 * safe to call from interrupt context (the timer tick, the NIC RX path). */
void entropy_stir(uint32_t sample);

/* Extract `n` bytes of hashed pool output, folding each block back into the
 * pool so the same bytes are never produced twice (forward secrecy). */
void entropy_get(void *out, uint32_t n);

#endif /* OWOS_ENTROPY_H */

/* io.h — x86 port I/O primitives. */
#ifndef OWOS_IO_H
#define OWOS_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) { __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port)); }

static inline uint8_t inb(uint16_t port)
{
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outw(uint16_t port, uint16_t val) { __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port)); }

static inline uint16_t inw(uint16_t port)
{
  uint16_t ret;
  __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outl(uint16_t port, uint32_t val) { __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port)); }

static inline uint32_t inl(uint16_t port)
{
  uint32_t ret;
  __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

/* Block port I/O: transfer `count` 16-bit words to/from `port` in one string
 * instruction. QEMU services a whole `rep insw`/`outsw` as a single bulk
 * transfer, vs. one VM-exit per word for a 256× inw loop — the difference
 * between ~10 s and a blink when streaming a megabyte off PIO IDE. The flat
 * kernel data segment makes ES:EDI / DS:ESI address kernel buffers directly;
 * `cld` pins the forward direction the SysV ABI already guarantees. */
static inline void insw(uint16_t port, void *buf, uint32_t count)
{
  __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
  __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

/* Brief delay by bouncing a byte off an unused port — old PICs need a moment
 * to settle between command writes. */
static inline void io_wait(void) { __asm__ volatile("outb %0, $0x80" : : "a"((uint8_t)0)); }

#endif /* OWOS_IO_H */

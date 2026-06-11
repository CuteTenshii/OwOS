/* cpu.c — CPUID queries. */

#include "cpu.h"
#include "timer.h"
#include <stdint.h>

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
	__asm__ volatile ("cpuid"
	                  : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
	                  : "a"(leaf));
}

/* Store a register's 4 bytes without assuming buf is aligned. */
static void store4(char *dst, uint32_t v)
{
	dst[0] = (char)(v);
	dst[1] = (char)(v >> 8);
	dst[2] = (char)(v >> 16);
	dst[3] = (char)(v >> 24);
}

void cpu_vendor(char *buf13)
{
	uint32_t a, b, c, d;
	cpuid(0, &a, &b, &c, &d);   /* vendor string is EBX, EDX, ECX             */
	store4(buf13 + 0, b);
	store4(buf13 + 4, d);
	store4(buf13 + 8, c);
	buf13[12] = '\0';
}

void cpu_brand(char *buf49)
{
	uint32_t a, b, c, d;
	cpuid(0x80000000, &a, &b, &c, &d);   /* highest extended leaf            */
	if (a < 0x80000004) {                /* no brand string available        */
		cpu_vendor(buf49);
		return;
	}
	int off = 0;
	for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
		cpuid(leaf, &a, &b, &c, &d);
		store4(buf49 + off + 0,  a);
		store4(buf49 + off + 4,  b);
		store4(buf49 + off + 8,  c);
		store4(buf49 + off + 12, d);
		off += 16;
	}
	buf49[48] = '\0';

	/* The brand is a fixed 48-char field, space-padded: Intel tends to pad on
	 * the left ("   Intel(R)..."), AMD on the right ("...Processor      ").
	 * Trim both ends so callers can print it flush. */
	int start = 0;
	while (buf49[start] == ' ')
		start++;
	int end = 47;
	while (end >= start && buf49[end] == ' ')
		end--;
	int n = end - start + 1;
	for (int i = 0; i < n; i++)
		buf49[i] = buf49[start + i];
	buf49[n] = '\0';
}

void cpu_signature(struct cpu_signature *sig)
{
	uint32_t a, b, c, d;
	cpuid(1, &a, &b, &c, &d);
	uint32_t base_fam  = (a >> 8)  & 0xF;
	uint32_t model     = (a >> 4)  & 0xF;
	uint32_t ext_model = (a >> 16) & 0xF;
	uint32_t ext_fam   = (a >> 20) & 0xFF;
	uint32_t family    = base_fam;
	/* Per Intel/AMD: extended family adds in only when base family is 0xF;
	 * extended model shifts in for families 0x6 and 0xF. */
	if (base_fam == 0xF)
		family += ext_fam;
	if (base_fam == 0x6 || base_fam == 0xF)
		model += ext_model << 4;
	sig->family   = family;
	sig->model    = model;
	sig->stepping = a & 0xF;
}

void cpu_address_bits(uint32_t *phys, uint32_t *virt)
{
	uint32_t a, b, c, d;
	cpuid(0x80000000, &a, &b, &c, &d);   /* highest extended leaf               */
	if (a < 0x80000008) {                /* leaf absent: architectural default  */
		*phys = 32;
		*virt = 32;
		return;
	}
	cpuid(0x80000008, &a, &b, &c, &d);
	*phys = a & 0xFF;
	*virt = (a >> 8) & 0xFF;
}

/* One CPUID feature bit and the name it prints as. */
struct feature {
	uint32_t leaf;    /* CPUID leaf                                            */
	char     reg;     /* 'c' = ECX, 'd' = EDX                                  */
	uint8_t  bit;     /* bit position within that register                    */
	const char *name;
};

/* Curated set: the flags that matter for this kernel plus the familiar ones
 * from /proc/cpuinfo, in roughly canonical order. */
static const struct feature features[] = {
	{ 1, 'd',  0, "fpu"   }, { 1, 'd',  3, "pse"    }, { 1, 'd',  4, "tsc"  },
	{ 1, 'd',  5, "msr"   }, { 1, 'd',  6, "pae"    }, { 1, 'd',  9, "apic" },
	{ 1, 'd', 13, "pge"   }, { 1, 'd', 15, "cmov"   }, { 1, 'd', 19, "clflush" },
	{ 1, 'd', 23, "mmx"   }, { 1, 'd', 24, "fxsr"   }, { 1, 'd', 25, "sse" },
	{ 1, 'd', 26, "sse2"  }, { 1, 'd', 28, "htt"    },
	{ 1, 'c',  0, "sse3"  }, { 1, 'c',  1, "pclmulqdq" }, { 1, 'c',  9, "ssse3" },
	{ 1, 'c', 12, "fma"   }, { 1, 'c', 19, "sse4_1" }, { 1, 'c', 20, "sse4_2" },
	{ 1, 'c', 23, "popcnt"}, { 1, 'c', 25, "aes"    }, { 1, 'c', 26, "xsave" },
	{ 1, 'c', 28, "avx"   }, { 1, 'c', 30, "rdrand" }, { 1, 'c', 31, "hypervisor" },
	{ 0x80000001, 'd', 11, "syscall" }, { 0x80000001, 'd', 20, "nx" },
	{ 0x80000001, 'd', 26, "pdpe1gb" }, { 0x80000001, 'd', 29, "lm" },
	{ 0x80000001, 'c',  0, "lahf"    }, { 0x80000001, 'c',  5, "lzcnt" },
};

void cpu_features(char *buf, uint32_t size)
{
	if (size == 0)
		return;

	uint32_t max_ext;
	{
		uint32_t a, b, c, d;
		cpuid(0x80000000, &a, &b, &c, &d);
		max_ext = a;
	}

	uint32_t pos = 0;
	for (uint32_t i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
		const struct feature *f = &features[i];
		if (f->leaf >= 0x80000000 && f->leaf > max_ext)
			continue;            /* extended leaf this CPU doesn't implement   */

		uint32_t a, b, c, d;
		cpuid(f->leaf, &a, &b, &c, &d);
		uint32_t reg = (f->reg == 'c') ? c : d;
		if (!(reg & (1u << f->bit)))
			continue;

		/* Append " name" (leading space only after the first flag). */
		if (pos != 0 && pos + 1 < size)
			buf[pos++] = ' ';
		for (const char *p = f->name; *p && pos + 1 < size; p++)
			buf[pos++] = *p;
	}
	buf[pos] = '\0';
}

static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

uint32_t cpu_mhz(void)
{
	uint32_t a, b, c, d;
	cpuid(1, &a, &b, &c, &d);
	if (!(d & (1u << 4)))           /* no TSC                                  */
		return 0;

	/* Count TSC cycles across a fixed window of whole PIT ticks. timer_ticks()
	 * is strictly monotonic (unlike timer_us(), whose pending-IRQ heuristic can
	 * briefly run backwards), so the unsigned tick subtraction is safe. We
	 * sample the TSC right after a tick edge so the window is an exact integer
	 * number of ticks. 20 ticks == 200 ms: long enough for a stable average,
	 * short enough not to stall the shell noticeably. */
	const uint32_t window_ticks = 20;
	uint32_t edge = timer_ticks();
	while (timer_ticks() == edge)
		;                       /* align to the next tick edge             */
	uint32_t start = timer_ticks();
	uint64_t start_tsc = rdtsc();
	while (timer_ticks() - start < window_ticks)
		;
	uint64_t cycles = rdtsc() - start_tsc;

	uint32_t elapsed_us = window_ticks * (1000000u / TIMER_HZ);
	return (uint32_t)(cycles / elapsed_us);   /* cycles per us == MHz        */
}

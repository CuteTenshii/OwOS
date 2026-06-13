/* mtrr.c — force the linear framebuffer to write-combining via a variable MTRR.
 *
 * fb.c already requests WC for the framebuffer through PAT (enable_wc tags the
 * LFB's PTEs to select a WC PAT entry). But on x86 the *effective* memory type
 * of a page is the combination of its MTRR type and its PAT type, and the
 * more-uncached of the two wins (see Intel SDM Vol 3, Table 11-7). On real
 * UEFI hardware the firmware programs the MTRRs and commonly leaves the GOP
 * framebuffer's physical range uncached — either by the MTRR default type or an
 * explicit variable range — so PAT-WC is silently overridden and every store to
 * the framebuffer becomes its own slow bus transaction. That is what laggy
 * window-drag / cursor movement looks like.
 *
 * This programs a variable-range MTRR that marks the framebuffer span WC, so the
 * effective type is WC no matter what PAT or the firmware default said. The
 * kernel is single-core, so the multi-processor MTRR rendezvous does not apply —
 * we follow the single-processor update sequence from SDM 11.11.8.
 *
 * Everything here is a no-op (beyond the diagnostic dump) when the CPU has no
 * MTRRs or no WC type — e.g. plain QEMU, whose "VRAM" is host RAM and already
 * fast.
 */

#include <stdint.h>
#include "mtrr.h"
#include "serial.h"

/* MSR addresses (SDM Vol 4). */
#define IA32_MTRRCAP        0x0FE
#define IA32_MTRR_DEF_TYPE  0x2FF
#define IA32_MTRR_PHYSBASE0 0x200   /* PHYSBASEn = 0x200 + 2n, PHYSMASKn = +1 */

#define MTRR_TYPE_UC  0x00
#define MTRR_TYPE_WC  0x01
#define MTRR_TYPE_WT  0x04
#define MTRR_TYPE_WP  0x05
#define MTRR_TYPE_WB  0x06

#define DEF_TYPE_E    (1u << 11)    /* MTRRs enabled                            */
#define DEF_TYPE_FE   (1u << 10)    /* fixed-range MTRRs enabled                 */
#define MTRRCAP_WC    (1u << 10)    /* WC memory type supported                  */
#define PHYSMASK_V    (1u << 11)    /* variable range valid                      */
#define CR0_CD        (1u << 30)
#define CR0_NW        (1u << 29)
#define CR4_PGE       (1u << 7)

static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
	__asm__ volatile ("wrmsr" : : "a"((uint32_t)v), "d"((uint32_t)(v >> 32)),
	                              "c"(msr));
}

static inline uint32_t read_cr0(void)
{ uint32_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint32_t v)
{ __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uint32_t read_cr3(void)
{ uint32_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint32_t v)
{ __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline uint32_t read_cr4(void)
{ uint32_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint32_t v)
{ __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory"); }

/* --- tiny heapless serial logging, mirroring idt.c's panic builder --- */

static void s_hex64(uint64_t v)
{
	static const char h[] = "0123456789ABCDEF";
	char buf[19];
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 0; i < 16; i++)
		buf[2 + i] = h[(v >> ((15 - i) * 4)) & 0xF];
	buf[18] = 0;
	serial_write(buf);
}

static void s_hex(uint32_t v)
{
	static const char h[] = "0123456789ABCDEF";
	char buf[11];
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 0; i < 8; i++)
		buf[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
	buf[10] = 0;
	serial_write(buf);
}

static void s_dec(uint32_t v)
{
	char buf[11];
	int i = 10;
	buf[i--] = 0;
	if (!v) buf[i--] = '0';
	while (v) { buf[i--] = '0' + (v % 10); v /= 10; }
	serial_write(&buf[i + 1]);
}

static const char *type_name(uint32_t t)
{
	switch (t) {
	case MTRR_TYPE_UC: return "UC";
	case MTRR_TYPE_WC: return "WC";
	case MTRR_TYPE_WT: return "WT";
	case MTRR_TYPE_WP: return "WP";
	case MTRR_TYPE_WB: return "WB";
	default:           return "??";
	}
}

/* Bits of physical address the CPU implements; bounds the PHYSMASK we write.
 * From CPUID 80000008h EAX[7:0] when available, else the safe 36 (PSE-36). */
static uint32_t phys_addr_bits(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	                          : "a"(0x80000000u), "c"(0));
	if (a >= 0x80000008u) {
		__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
		                          : "a"(0x80000008u), "c"(0));
		uint32_t bits = a & 0xFF;
		if (bits >= 32 && bits <= 52)
			return bits;
	}
	return 36;
}

/* Does CPUID report MTRR support (CPUID.01H:EDX bit 12)? */
static int have_mtrr(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	                          : "a"(1u), "c"(0));
	return (d >> 12) & 1;
}

/* True if a *valid* variable MTRR `i` covers `phys`: (phys & mask) == (base &
 * mask), comparing only the address bits (mask without its flag bits). */
static int var_covers(uint32_t i, uint32_t phys)
{
	uint64_t base = rdmsr(IA32_MTRR_PHYSBASE0 + 2 * i);
	uint64_t mask = rdmsr(IA32_MTRR_PHYSBASE0 + 2 * i + 1);
	if (!(mask & PHYSMASK_V))
		return 0;
	uint64_t m = mask & ~0xFFFULL;
	return ((uint64_t)phys & m) == (base & m);
}

/* The variable MTRR memory type effective for `phys`, applying SDM 11.11.4.1
 * overlap precedence: any overlapping UC wins; else WT beats WB; else the first
 * match. Returns -1 when no variable range covers `phys` (the default type then
 * applies). `out_uc_slot` (if non-NULL) receives the index of an overlapping UC
 * range, or -1 — so the caller can tell "UC from default" apart from "UC from a
 * variable range" (only the former is fixable by adding a WC range). */
static int mtrr_var_type(uint32_t phys, uint32_t vcnt, int *out_uc_slot)
{
	int found = -1, saw_wt = 0, saw_wb = 0, uc_slot = -1;
	for (uint32_t i = 0; i < vcnt; i++) {
		if (!var_covers(i, phys))
			continue;
		int t = (int)(rdmsr(IA32_MTRR_PHYSBASE0 + 2 * i) & 0xFF);
		if (t == MTRR_TYPE_UC && uc_slot < 0) uc_slot = (int)i;
		if (t == MTRR_TYPE_WT) saw_wt = 1;
		if (t == MTRR_TYPE_WB) saw_wb = 1;
		if (found < 0) found = t;
	}
	if (out_uc_slot) *out_uc_slot = uc_slot;
	if (uc_slot >= 0)        return MTRR_TYPE_UC;
	if (saw_wt && saw_wb)    return MTRR_TYPE_WT;
	return found;
}

/* The page-level effective type for the framebuffer, whose PTEs select PAT-WC,
 * combined with its MTRR type (SDM Table 11-7, WC PAT column): UC MTRR forces
 * UC (slow); WB or WC MTRR yields WC (fast); anything else we treat as not-WC. */
static int fb_effective_wc(uint32_t phys, uint32_t vcnt, uint32_t def_type,
                           int *out_uc_slot)
{
	int mt = mtrr_var_type(phys, vcnt, out_uc_slot);
	if (mt < 0)                                  /* no variable range: default  */
		mt = (int)(def_type & 0xFF);
	return mt == MTRR_TYPE_WB || mt == MTRR_TYPE_WC;
}

/* Dump current MTRR/PAT state and the framebuffer's effective type to COM1. */
static void dump_state(uint32_t phys, uint32_t vcnt, uint32_t def_type,
                       const char *when)
{
	serial_write("[mtrr] "); serial_write(when); serial_write(":\n");
	serial_write("[mtrr]   fb phys     = "); s_hex(phys); serial_write("\n");
	serial_write("[mtrr]   IA32_PAT    = "); s_hex64(rdmsr(0x277));
	serial_write("\n");
	serial_write("[mtrr]   DEF_TYPE    = "); s_hex(def_type & 0xFFFF);
	serial_write("  (default ");
	serial_write(type_name(def_type & 0xFF));
	serial_write((def_type & DEF_TYPE_E) ? ", MTRRs ON)" : ", MTRRs OFF)");
	serial_write("\n");
	for (uint32_t i = 0; i < vcnt; i++) {
		uint64_t base = rdmsr(IA32_MTRR_PHYSBASE0 + 2 * i);
		uint64_t mask = rdmsr(IA32_MTRR_PHYSBASE0 + 2 * i + 1);
		if (!(mask & PHYSMASK_V))
			continue;
		serial_write("[mtrr]   var["); s_dec(i); serial_write("] base=");
		s_hex64(base & ~0xFFFULL);
		serial_write(" mask="); s_hex64(mask & ~0xFFFULL);
		serial_write(" "); serial_write(type_name(base & 0xFF));
		serial_write("\n");
	}
	int uc_slot;
	int mt = mtrr_var_type(phys, vcnt, &uc_slot);
	serial_write("[mtrr]   fb MTRR type= ");
	serial_write(mt < 0 ? "default" : type_name(mt));
	serial_write("\n[mtrr]   fb effective= ");
	if (fb_effective_wc(phys, vcnt, def_type, &uc_slot))
		serial_write("WC  (write-combining active)");
	else if (uc_slot >= 0) {
		serial_write("UC  (PAT-WC overridden by UC var[");
		s_dec((uint32_t)uc_slot);
		serial_write("] -> framebuffer is SLOW)");
	} else
		serial_write("UC  (PAT-WC overridden by default type -> SLOW)");
	serial_write("\n");
}

/* Find a free variable MTRR slot (valid bit clear), or -1 if all are in use. */
static int free_slot(uint32_t vcnt)
{
	for (uint32_t i = 0; i < vcnt; i++)
		if (!(rdmsr(IA32_MTRR_PHYSBASE0 + 2 * i + 1) & PHYSMASK_V))
			return (int)i;
	return -1;
}

int mtrr_set_wc(uint32_t phys, uint32_t len)
{
	if (!have_mtrr()) {
		serial_write("[mtrr] no MTRR support (CPUID) — PAT only\n");
		return -1;
	}

	uint64_t cap      = rdmsr(IA32_MTRRCAP);
	uint32_t vcnt     = (uint32_t)(cap & 0xFF);
	uint64_t def      = rdmsr(IA32_MTRR_DEF_TYPE);
	uint32_t def_type = (uint32_t)def;

	dump_state(phys, vcnt, def_type, "before");

	if (!(cap & MTRRCAP_WC)) {
		serial_write("[mtrr] CPU lacks WC type — PAT only\n");
		return -1;
	}

	/* Express [phys, phys+len) as one power-of-two, naturally aligned range —
	 * the only shape a single variable MTRR can describe. size = smallest power
	 * of two >= len; then phys must be size-aligned. Real GPU framebuffer BARs
	 * are aligned to far more than the rounded fb size, so this holds in
	 * practice; if not, we bail (leaving PAT-only) rather than mark a wrong
	 * span. The extra bytes between len and size sit inside the GPU aperture and
	 * are harmless to mark WC. */
	uint32_t size = 0x1000;
	while (size < len)
		size <<= 1;
	if (phys & (size - 1)) {
		serial_write("[mtrr] fb base not aligned to its size (");
		s_hex(size); serial_write(") — PAT only\n");
		return -1;
	}

	/* Already effectively write-combining? Two ways this happens without us:
	 * an existing WC variable range, or a WB/default region (WB MTRR + PAT-WC
	 * combines to WC). Either way the framebuffer is already fast — nothing to
	 * do, and adding a redundant MTRR would only burn a slot. */
	int uc_slot;
	if (fb_effective_wc(phys, vcnt, def_type, &uc_slot)) {
		serial_write("[mtrr] fb already effectively WC — no change needed\n");
		return 0;
	}

	/* It's UC. We can only force WC by *adding* a WC range when the UC comes
	 * from the default type. If a UC *variable* range overlaps the fb, the
	 * overlap rule (SDM 11.11.4.1) makes UC win regardless of any WC range we
	 * add — the firmware's UC range would have to be retiled around the fb,
	 * which we don't do (it also covers MMIO that must stay UC). Report and stop
	 * rather than install a WC range that silently does nothing. */
	if (uc_slot >= 0) {
		serial_write("[mtrr] fb is inside UC var[");
		s_dec((uint32_t)uc_slot);
		serial_write("]; an added WC range can't override an overlapping UC "
		             "range. Firmware MTRR layout needs retiling (not done). "
		             "PAT only.\n");
		return -1;
	}

	int slot = free_slot(vcnt);
	if (slot < 0) {
		serial_write("[mtrr] no free variable MTRR slot — PAT only\n");
		return -1;
	}

	uint32_t pab      = phys_addr_bits();
	uint64_t addr_mask = (pab >= 64) ? ~0ULL : ((1ULL << pab) - 1);
	uint64_t physbase = ((uint64_t)phys & addr_mask & ~0xFFFULL) | MTRR_TYPE_WC;
	uint64_t physmask = (~((uint64_t)size - 1) & addr_mask) | PHYSMASK_V;

	serial_write("[mtrr] installing WC: slot="); s_dec((uint32_t)slot);
	serial_write(" base="); s_hex(phys);
	serial_write(" size="); s_hex(size);
	serial_write(" (physmask="); s_hex64(physmask); serial_write(")\n");

	/* Single-processor MTRR update sequence (SDM 11.11.8): enter no-fill cache
	 * mode, flush, disable MTRRs, edit, flush, re-enable, restore. IF is cleared
	 * throughout so nothing runs against half-changed cache control. */
	uint32_t flags;
	__asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) :: "memory");

	uint32_t cr0 = read_cr0();
	write_cr0((cr0 & ~CR0_NW) | CR0_CD);     /* CD=1, NW=0: no-fill mode        */
	__asm__ volatile ("wbinvd" ::: "memory");

	uint32_t cr4 = read_cr4();
	if (cr4 & CR4_PGE)
		write_cr4(cr4 & ~CR4_PGE);           /* flush global TLB entries        */
	else
		write_cr3(read_cr3());

	wrmsr(IA32_MTRR_DEF_TYPE, def & ~(uint64_t)DEF_TYPE_E);   /* MTRRs off      */

	wrmsr(IA32_MTRR_PHYSBASE0 + 2 * slot,     physbase);
	wrmsr(IA32_MTRR_PHYSBASE0 + 2 * slot + 1, physmask);

	__asm__ volatile ("wbinvd" ::: "memory");
	if (cr4 & CR4_PGE)
		write_cr4(cr4 & ~CR4_PGE);
	else
		write_cr3(read_cr3());

	wrmsr(IA32_MTRR_DEF_TYPE, def | DEF_TYPE_E);   /* MTRRs on, same default    */

	write_cr0(cr0);                          /* CD/NW back to entry values      */
	if (cr4 & CR4_PGE)
		write_cr4(cr4);                      /* restore PGE                     */

	if (flags & 0x200)
		__asm__ volatile ("sti");

	dump_state(phys, vcnt, (uint32_t)rdmsr(IA32_MTRR_DEF_TYPE), "after");

	/* Confirm we actually achieved WC; if not, the slot did nothing useful —
	 * roll it back so we don't leave a stray range behind. */
	if (fb_effective_wc(phys, vcnt, def_type, &uc_slot)) {
		serial_write("[mtrr] framebuffer is now WC\n");
		return 0;
	}
	serial_write("[mtrr] WC not achieved after install — rolling back slot\n");
	wrmsr(IA32_MTRR_PHYSBASE0 + 2 * slot + 1, 0);   /* clear valid bit          */
	return -1;
}

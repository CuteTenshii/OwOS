/* edid.c — read a monitor's EDID over Intel GMBUS, recover its refresh rate.
 *
 * Why GMBUS and nothing else: there is no portable, BIOS-free way to reach the
 * DDC/I2C lines that carry EDID — each GPU family exposes its own I2C engine.
 * Intel's "GMBUS" is a clean MMIO controller that has stayed largely stable
 * across generations, and Intel integrated graphics is what a hobby kernel will
 * actually meet on a laptop, so it is the one transport worth implementing. We
 * walk the candidate DDC pin pairs, do a single I2C read of 128 bytes from the
 * EDID EEPROM at slave address 0x50, validate the block, and derive the refresh
 * rate from the preferred detailed timing descriptor.
 *
 * This cannot be exercised under QEMU (its emulated VGA answers no EDID over
 * GMBUS), so it is real-hardware-only and best-effort throughout: every failure
 * path records edid_error() and returns 0/empty rather than faulting. Known
 * gaps, by design: non-Intel GPUs, and internal eDP panels (EDID over the DP
 * AUX channel, not I2C). See edid.h.
 */

#include <stdint.h>
#include "edid.h"
#include "pci.h"
#include "paging.h"
#include "timer.h"

/* GMBUS register block, offsets from the block base. */
#define GMBUS0 0x00   /* clock / pin-pair select                              */
#define GMBUS1 0x04   /* command + status                                     */
#define GMBUS2 0x08   /* status                                               */
#define GMBUS3 0x0C   /* 4-byte data FIFO                                      */

#define GMBUS_RATE_100KHZ      (0u << 8)
#define GMBUS_SW_CLR_INT       (1u << 31)
#define GMBUS_SW_RDY           (1u << 30)
#define GMBUS_CYCLE_WAIT       (1u << 25)
#define GMBUS_CYCLE_STOP       (4u << 25)
#define GMBUS_BYTE_COUNT_SHIFT 16
#define GMBUS_SLAVE_READ       (1u << 0)

#define GMBUS_HW_RDY           (1u << 11)   /* a data word is ready in GMBUS3  */
#define GMBUS_SATOER           (1u << 10)   /* slave NAK / no monitor on pin   */

#define EDID_ADDR  0x50   /* DDC EEPROM I2C slave address                      */
#define EDID_LEN   128    /* one EDID block                                    */

/* The GMBUS block sits at different offsets within BAR0 across generations: the
 * PCH-split parts (Gen5+) at 0xC5100, older integrated parts at 0x5100. We try
 * both. (VLV/CHV's 0x180000 display offset is left out — niche, and untestable
 * here anyway.) */
static const uint32_t gmbus_bases[] = { 0xC5100, 0x5100 };

/* Candidate DDC pin pairs (GMBUS0[2:0]). The pin->port mapping shifts between
 * generations, so rather than decode it we just try the usual suspects —
 * analog/VGA and the digital ports — and take whichever answers with a valid
 * block. */
static const uint8_t gmbus_pins[] = { 2, 4, 5, 6, 3, 1 };

static volatile uint8_t *mmio;       /* mapped GPU BAR0                         */
static uint32_t          base;       /* current GMBUS block offset             */

static int  probed;                  /* one-shot guard                         */
static int  cached_hz, cached_w, cached_h;
static char err[48];

static void seterr(const char *s)
{
	int i = 0;
	while (s[i] && i < (int)sizeof(err) - 1) {
		err[i] = s[i];
		i++;
	}
	err[i] = '\0';
}

static inline uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(mmio + base + off);
}
static inline void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(mmio + base + off) = val;
}

/* Locate the first Intel display controller (vendor 0x8086, PCI class 0x03) and
 * return its BAR0 physical base. Integrated graphics always lives on bus 0, so
 * a single-bus scan is enough. Returns 0 on success. */
static int find_intel_gpu(uint32_t *bar_out)
{
	for (uint8_t slot = 0; slot < 32; slot++) {
		uint8_t header = (pci_read32(0, slot, 0, 0x0C) >> 16) & 0xFF;
		int funcs = (header & 0x80) ? 8 : 1;
		for (int func = 0; func < funcs; func++) {
			uint32_t id = pci_read32(0, slot, func, 0x00);
			if ((id & 0xFFFF) != 0x8086)
				continue;
			uint32_t cls = pci_read32(0, slot, func, 0x08);
			if (((cls >> 24) & 0xFF) != 0x03)     /* 0x03 = display          */
				continue;

			uint32_t bar0 = pci_read32(0, slot, func, 0x10);
			if (((bar0 >> 1) & 0x3) == 0x2) {     /* 64-bit memory BAR       */
				if (pci_read32(0, slot, func, 0x14) != 0) {
					seterr("GPU BAR above 4GB");
					return -1;
				}
			}
			*bar_out = bar0 & ~0xFu;
			return 0;
		}
	}
	seterr("no Intel GPU");
	return -1;
}

/* Single GMBUS I2C read: `len` bytes from `addr` on the selected `pin` into buf.
 * The DDC EEPROM's address pointer powers up at 0, so a bare read returns the
 * block from byte 0. Returns 0 on success, -1 on NAK, -2 on timeout. */
static int gmbus_read(uint8_t pin, uint8_t addr, uint8_t *buf, int len)
{
	wr(GMBUS1, GMBUS_SW_CLR_INT);                 /* clear any stale state    */
	wr(GMBUS1, 0);
	wr(GMBUS0, pin | GMBUS_RATE_100KHZ);

	wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_WAIT | GMBUS_CYCLE_STOP |
	           ((uint32_t)len << GMBUS_BYTE_COUNT_SHIFT) |
	           ((uint32_t)addr << 1) | GMBUS_SLAVE_READ);

	int got = 0;
	while (got < len) {
		uint32_t start = timer_us();
		for (;;) {
			uint32_t st = rd(GMBUS2);
			if (st & GMBUS_SATOER) {              /* no monitor on this pin  */
				wr(GMBUS0, 0);
				return -1;
			}
			if (st & GMBUS_HW_RDY)
				break;
			if (timer_us() - start > 50000) {     /* 50 ms guard             */
				wr(GMBUS0, 0);
				return -2;
			}
		}
		uint32_t word = rd(GMBUS3);
		for (int b = 0; b < 4 && got < len; b++)
			buf[got++] = (word >> (b * 8)) & 0xFF;
	}
	wr(GMBUS0, 0);                                /* release the bus          */
	return 0;
}

/* EDID header is 00 FF FF FF FF FF FF 00, and the 128 bytes sum to 0 mod 256. */
static int edid_valid(const uint8_t *e)
{
	static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
	for (int i = 0; i < 8; i++)
		if (e[i] != hdr[i])
			return 0;
	uint8_t sum = 0;
	for (int i = 0; i < EDID_LEN; i++)
		sum += e[i];
	return sum == 0;
}

/* Derive width/height/refresh from the first detailed timing descriptor (the
 * preferred mode), at offset 54. refresh = pixel_clock / (htotal * vtotal). */
static int parse_preferred(const uint8_t *e)
{
	const uint8_t *d = e + 54;
	uint32_t pclk = d[0] | ((uint32_t)d[1] << 8);   /* 10 kHz units           */
	if (pclk == 0) {                                /* not a timing descriptor */
		seterr("no preferred DTD");
		return -1;
	}
	uint32_t hact = d[2] | (((uint32_t)d[4] >> 4) << 8);
	uint32_t hbl  = d[3] | (((uint32_t)d[4] & 0x0F) << 8);
	uint32_t vact = d[5] | (((uint32_t)d[7] >> 4) << 8);
	uint32_t vbl  = d[6] | (((uint32_t)d[7] & 0x0F) << 8);

	uint32_t htotal = hact + hbl;
	uint32_t vtotal = vact + vbl;
	if (htotal == 0 || vtotal == 0) {
		seterr("bad DTD totals");
		return -1;
	}
	uint32_t total = htotal * vtotal;
	cached_hz = (int)((pclk * 10000u + total / 2) / total);   /* rounded Hz   */
	cached_w  = (int)hact;
	cached_h  = (int)vact;
	return 0;
}

static void probe(void)
{
	probed = 1;
	err[0] = '\0';

	uint32_t bar;
	if (find_intel_gpu(&bar) != 0)
		return;

	/* The GMBUS block lives in the first ~1 MiB of BAR0 (0x5100 / 0xC5100);
	 * map that much uncached. The map is one-shot (not reclaimed), like the
	 * ACPI table maps. */
	if (paging_identity_map(bar, 0x100000) != 0) {
		seterr("BAR map failed");
		return;
	}
	paging_mark_uc(bar, 0x100000);
	mmio = (volatile uint8_t *)bar;

	uint8_t edid[EDID_LEN];
	for (uint32_t bi = 0; bi < sizeof(gmbus_bases) / sizeof(gmbus_bases[0]); bi++) {
		base = gmbus_bases[bi];
		for (uint32_t pi = 0; pi < sizeof(gmbus_pins); pi++) {
			if (gmbus_read(gmbus_pins[pi], EDID_ADDR, edid, EDID_LEN) != 0)
				continue;
			if (!edid_valid(edid)) {
				seterr("bad checksum");
				continue;
			}
			if (parse_preferred(edid) == 0)
				return;                           /* success                 */
		}
	}
	if (err[0] == '\0')
		seterr("no EDID (GMBUS NAK)");
}

static void ensure(void)
{
	if (!probed)
		probe();
}

int edid_refresh_hz(void) { ensure(); return cached_hz; }
int edid_width(void)      { ensure(); return cached_w;  }
int edid_height(void)     { ensure(); return cached_h;  }

const char *edid_error(void)
{
	ensure();
	return err;
}

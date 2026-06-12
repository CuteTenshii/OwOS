/* ata.c — primary-master ATA disk access via PIO, polled (no IRQ14).
 *
 * Polling keeps this simple and self-contained. Every wait loop is bounded so
 * a missing or wedged drive returns an error instead of hanging the kernel.
 */

#include "ata.h"
#include "io.h"

#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7   /* read: status; write: command                  */
#define ATA_CTRL    0x3F6   /* alternate status (no side effects on read)     */

#define ST_BSY  0x80
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ  0x20
#define CMD_WRITE 0x30
#define CMD_FLUSH 0xE7

#define POLL_LIMIT 1000000u

/* Reading the alternate status 4x gives the drive the ~400ns it needs after
 * a command before its status is meaningful. */
static void ata_delay(void)
{
	for (int i = 0; i < 4; i++)
		(void)inb(ATA_CTRL);
}

static int wait_not_busy(void)
{
	for (uint32_t i = 0; i < POLL_LIMIT; i++)
		if (!(inb(ATA_STATUS) & ST_BSY))
			return 0;
	return -1;
}

/* Wait for the drive to be ready to transfer a sector (DRQ set, no error). */
static int wait_drq(void)
{
	ata_delay();
	if (wait_not_busy())
		return -1;
	for (uint32_t i = 0; i < POLL_LIMIT; i++) {
		uint8_t s = inb(ATA_STATUS);
		if (s & ST_ERR)
			return -1;
		if (s & ST_DRQ)
			return 0;
	}
	return -1;
}

/* Start a transfer of `count` sectors (1..256) at `lba`. The SECCNT register is
 * 8-bit and reads 0 as 256, so (uint8_t)256 == 0 is exactly right for a
 * full-block transfer. */
static int issue(uint32_t lba, uint8_t cmd, uint32_t count)
{
	if (wait_not_busy())
		return -1;
	outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));  /* master, LBA mode        */
	outb(ATA_SECCNT, (uint8_t)count);
	outb(ATA_LBA0, lba & 0xFF);
	outb(ATA_LBA1, (lba >> 8) & 0xFF);
	outb(ATA_LBA2, (lba >> 16) & 0xFF);
	outb(ATA_STATUS, cmd);
	return 0;
}

/* Read `count` sectors into buf. One READ SECTORS command covers up to 256
 * sectors (the LBA28 limit); the drive raises DRQ once per sector, and each is
 * drained with a single `rep insw`. Batching this way turns the old
 * 256-inw-per-sector loop (a VM-exit per word) into one bulk transfer per
 * sector and amortizes the command setup across the whole run. */
int ata_read(uint32_t lba, void *buf, uint32_t count)
{
	uint8_t *p = buf;
	while (count > 0) {
		uint32_t n = count > 256 ? 256 : count;
		if (issue(lba, CMD_READ, n))
			return -1;
		for (uint32_t s = 0; s < n; s++) {
			if (wait_drq())
				return -1;
			insw(ATA_DATA, p, 256);
			p += 512;
		}
		lba   += n;
		count -= n;
	}
	return 0;
}

int ata_read_sector(uint32_t lba, void *buf)
{
	return ata_read(lba, buf, 1);
}

int ata_write_sector(uint32_t lba, const void *buf)
{
	if (issue(lba, CMD_WRITE, 1) || wait_drq())
		return -1;
	outsw(ATA_DATA, buf, 256);
	outb(ATA_STATUS, CMD_FLUSH);       /* commit to the platter               */
	return wait_not_busy();
}

#define CMD_IDENTIFY 0xEC

/* Run IDENTIFY DEVICE on the primary master into a 256-word buffer. Returns 0
 * and fills words[] on success, -1 if no drive responds. */
static int identify(uint16_t *words)
{
	/* Floating-bus check. With no IDE controller decoding these ports (e.g. an
	 * AHCI/NVMe-only laptop), reads return 0xFF. Without this we'd mistake the
	 * stuck-high 0xFF (BSY set) for a perpetually-busy drive and spin the full
	 * POLL_LIMIT on every probe — ~5s of dead port reads before concluding "no
	 * drive". Bail immediately instead. */
	if (inb(ATA_STATUS) == 0xFF)
		return -1;

	if (wait_not_busy())
		return -1;
	outb(ATA_DRIVE, 0xA0);                  /* select master                    */
	outb(ATA_SECCNT, 0);
	outb(ATA_LBA0, 0);
	outb(ATA_LBA1, 0);
	outb(ATA_LBA2, 0);
	outb(ATA_STATUS, CMD_IDENTIFY);

	if (inb(ATA_STATUS) == 0)               /* status 0 = no drive present      */
		return -1;
	if (wait_drq())
		return -1;
	for (int i = 0; i < 256; i++)
		words[i] = inw(ATA_DATA);
	return 0;
}

int ata_present(void)
{
	uint16_t words[256];
	return identify(words) == 0;
}

uint32_t ata_sectors(void)
{
	uint16_t words[256];
	if (identify(words))
		return 0;
	/* Words 60-61: total addressable sectors in LBA28 mode. */
	return (uint32_t)words[60] | ((uint32_t)words[61] << 16);
}

/* ATA IDENTIFY strings are stored big-endian within each 16-bit word: the high
 * byte is the first character. Copy `count` words out, swapping, then trim
 * trailing spaces. */
static void ata_extract_string(char *dst, const uint16_t *words, int base, int count)
{
	int n = 0;
	for (int i = 0; i < count; i++) {
		dst[n++] = (char)(words[base + i] >> 8);
		dst[n++] = (char)(words[base + i] & 0xFF);
	}
	while (n > 0 && dst[n - 1] == ' ')
		n--;
	dst[n] = '\0';
}

const char *ata_model(void)
{
	static char model[41];
	uint16_t words[256];
	if (identify(words)) {
		model[0] = '\0';
		return model;
	}
	ata_extract_string(model, words, 27, 20);   /* words 27-46: model number    */
	return model;
}

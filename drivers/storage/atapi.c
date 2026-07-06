/* atapi.c — polled ATAPI (CD/DVD) access via the PACKET command, no IRQ.
 *
 * Like ata.c this polls with bounded loops so a missing or wedged drive returns
 * an error instead of hanging the kernel. The difference is the transport: every
 * data access is a 12-byte SCSI command descriptor block (CDB) handed to the
 * drive with the ATA PACKET (0xA0) command, after which the drive does the read
 * and PIO-streams 2048-byte logical sectors back over the data port.
 */

#include "atapi.h"
#include "io.h"
#include "console.h"

/* Per-channel I/O register file. Two legacy IDE channels, each with a command
 * block at `io` and an alternate-status/device-control byte at `ctrl`. */
struct chan { uint16_t io, ctrl; };
static const struct chan chans[2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };

/* Command-block register offsets from `io`. */
#define R_DATA     0
#define R_FEATURES 1          /* write: features (0 = PIO, no DMA/overlap)      */
#define R_ERROR    1          /* read: error                                    */
#define R_IREASON  2          /* interrupt reason / sector count                */
#define R_LBA0     3
#define R_BCOUNT_L 4          /* byte-count limit low  (a.k.a. LBA mid)         */
#define R_BCOUNT_H 5          /* byte-count limit high (a.k.a. LBA high)        */
#define R_DRIVE    6
#define R_STATUS   7          /* read: status; write: command                   */

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01          /* on ATAPI this is the CHK (check-condition) bit */

#define CMD_IDENTIFY        0xEC    /* plain ATA IDENTIFY (ATAPI aborts it)     */
#define CMD_IDENTIFY_PACKET 0xA1    /* IDENTIFY PACKET DEVICE                   */
#define CMD_PACKET          0xA0    /* send a SCSI CDB                          */

/* SCSI op codes used inside the 12-byte CDB. */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ10          0x28
#define SCSI_INQUIRY         0x12

#define POLL_LIMIT 1000000u
#define MAX_PACKET_BYTES 65536u   /* cap the DRQ stream: a wedged drive can keep
                                     re-asserting DRQ forever (largest legit reply
                                     is a 2048-byte sector, so this is ample)      */

/* The first CD/DVD drive atapi_init() latched onto, if any. One is plenty —
 * mirrors ata.c's single-disk model. */
static struct {
	int      present;
	uint16_t io, ctrl;
	uint8_t  sel;        /* 0xA0 = master, 0xB0 = slave                        */
	char     model[41];
} cd;

/* Read the alternate status four times (~400 ns) so the drive's status is
 * meaningful after a command/select. */
static void atapi_delay(uint16_t ctrl)
{
	for (int i = 0; i < 4; i++)
		(void)inb(ctrl);
}

static int wait_not_busy(uint16_t io)
{
	for (uint32_t i = 0; i < POLL_LIMIT; i++)
		if (!(inb(io + R_STATUS) & ST_BSY))
			return 0;
	return -1;
}

/* Wait until the drive is ready to move a phase along: BSY clear and either DRQ
 * set (data/command bytes wanted) or ERR set. Returns 0 with DRQ up, -1 on
 * error/timeout. */
static int wait_drq(uint16_t io)
{
	for (uint32_t i = 0; i < POLL_LIMIT; i++) {
		uint8_t s = inb(io + R_STATUS);
		if (s & ST_BSY)
			continue;
		if (s & ST_ERR)
			return -1;
		if (s & ST_DRQ)
			return 0;
	}
	return -1;
}

/* Select master/slave on a channel and give it the settle time it needs. */
static void select_drive(uint16_t io, uint16_t ctrl, uint8_t sel)
{
	outb(io + R_DRIVE, sel);
	atapi_delay(ctrl);
}

/* Run one PACKET command: hand the drive a 12-byte CDB, then PIO-read up to
 * `buflen` bytes of its reply. `buflen` also caps the per-DRQ byte-count the
 * drive may use. Returns total bytes read, or -1 on error.
 *
 * Drives report their reply in one or more DRQ bursts; each burst's size is in
 * the byte-count registers and need not match what we asked for, so the loop
 * reads whatever the drive offers and stops when DRQ drops. */
static int packet(const uint8_t cdb[12], void *buf, uint32_t buflen)
{
	uint16_t io = cd.io, ctrl = cd.ctrl;

	select_drive(io, ctrl, cd.sel);
	if (wait_not_busy(io))
		return -1;

	outb(io + R_FEATURES, 0x00);                       /* PIO, no DMA          */
	outb(io + R_IREASON,  0x00);
	outb(io + R_BCOUNT_L, (uint8_t)(buflen & 0xFF));    /* byte-count limit     */
	outb(io + R_BCOUNT_H, (uint8_t)(buflen >> 8));
	outb(io + R_STATUS,   CMD_PACKET);

	if (wait_drq(io))                                  /* drive wants the CDB   */
		return -1;
	outsw(io + R_DATA, cdb, 6);                        /* 12 bytes = 6 words    */

	uint8_t  *p   = buf;
	uint32_t  got = 0;
	for (;;) {
		if (got > MAX_PACKET_BYTES)                       /* wedged drive          */
			return -1;
		/* Settle ~400 ns first: right after the CDB (and after each data block)
		 * the drive needs a moment to raise BSY before its status is meaningful.
		 * Without this the first read sees stale BSY=0/DRQ=0 and quits early. */
		atapi_delay(ctrl);
		if (wait_not_busy(io))
			return -1;
		uint8_t s = inb(io + R_STATUS);
		if (s & ST_ERR)
			return -1;
		if (!(s & ST_DRQ))
			break;                                    /* transfer complete     */

		uint32_t cnt = (uint32_t)inb(io + R_BCOUNT_L) |
		               ((uint32_t)inb(io + R_BCOUNT_H) << 8);
		if (cnt == 0)
			break;
		/* ATAPI byte counts are always even. Drain into the caller's buffer
		 * while it has room; read and discard any surplus to keep the bus in
		 * sync with the drive's view of the transfer. */
		for (uint32_t i = 0; i < cnt; i += 2) {
			uint16_t w = inw(io + R_DATA);
			if (got + 1 < buflen) {
				p[got]     = (uint8_t)w;
				p[got + 1] = (uint8_t)(w >> 8);
			}
			got += 2;
		}
	}
	return (int)got;
}

/* IDENTIFY-string fields are stored big-endian within each 16-bit word, like
 * plain ATA: high byte first. Copy `count` words out, byte-swapped, then trim
 * trailing spaces. */
static void extract_string(char *dst, const uint16_t *words, int base, int count)
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

/* Probe one (channel, drive) position. If an ATAPI device answers, fill `cd`
 * and return 1. The detection is the standard one: a plain ATA IDENTIFY aborts
 * on a packet device, which leaves the ATAPI signature 0x14/0xEB in the byte-
 * count (LBA mid/high) registers. */
static int probe_one(uint16_t io, uint16_t ctrl, uint8_t sel)
{
	/* Floating bus: no controller decoding these ports reads back 0xFF. Bail so
	 * we don't mistake stuck-high BSY for a busy drive and burn POLL_LIMIT. */
	if (inb(io + R_STATUS) == 0xFF)
		return 0;

	select_drive(io, ctrl, sel);

	outb(io + R_IREASON,  0);
	outb(io + R_BCOUNT_L, 0);
	outb(io + R_BCOUNT_H, 0);
	outb(io + R_LBA0,     0);
	outb(io + R_STATUS,   CMD_IDENTIFY);

	if (inb(io + R_STATUS) == 0)            /* status 0 = nothing here          */
		return 0;
	atapi_delay(ctrl);
	if (wait_not_busy(io))
		return 0;

	uint8_t sig_lo = inb(io + R_BCOUNT_L);
	uint8_t sig_hi = inb(io + R_BCOUNT_H);
	if (sig_lo != 0x14 || sig_hi != 0xEB)   /* not the ATAPI signature          */
		return 0;

	/* It's a packet device. Pull its model via IDENTIFY PACKET DEVICE. */
	cd.present = 1;
	cd.io = io;
	cd.ctrl = ctrl;
	cd.sel = sel;
	cd.model[0] = '\0';

	select_drive(io, ctrl, sel);
	outb(io + R_STATUS, CMD_IDENTIFY_PACKET);
	if (wait_drq(io) == 0) {
		uint16_t words[256];
		for (int i = 0; i < 256; i++)
			words[i] = inw(io + R_DATA);
		extract_string(cd.model, words, 27, 20);   /* words 27-46: model       */
	}
	return 1;
}

void atapi_init(void)
{
	cd.present = 0;
	for (int c = 0; c < 2 && !cd.present; c++)
		for (int d = 0; d < 2; d++) {
			uint8_t sel = d ? 0xB0 : 0xA0;
			if (probe_one(chans[c].io, chans[c].ctrl, sel))
				break;
		}
	if (cd.present) {
		con_write("atapi: CD/DVD drive (");
		con_write(cd.model);
		con_write(")\n");
	}
}

int atapi_present(void)
{
	return cd.present;
}

const char *atapi_model(void)
{
	return cd.present ? cd.model : "";
}

/* TEST UNIT READY: 0 if a medium is loaded and the drive is ready. After a media
 * change the first call typically returns a "unit attention" check-condition;
 * the drive is ready by the next, so callers retry. */
static int test_unit_ready(void)
{
	uint8_t cdb[12] = { SCSI_TEST_UNIT_READY };
	return packet(cdb, 0, 0) < 0 ? -1 : 0;
}

uint32_t atapi_sectors(void)
{
	if (!cd.present)
		return 0;

	/* Nudge the drive past unit-attention / spin-up before trusting capacity. */
	for (int i = 0; i < 4 && test_unit_ready() != 0; i++)
		;

	uint8_t cdb[12] = { SCSI_READ_CAPACITY };
	uint8_t resp[8];
	if (packet(cdb, resp, sizeof resp) < 8)
		return 0;
	/* READ CAPACITY(10): bytes 0-3 = LBA of the last block, big-endian. */
	uint32_t last = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
	                ((uint32_t)resp[2] << 8)  |  (uint32_t)resp[3];
	return last + 1;
}

int atapi_read_sector(uint32_t lba, void *buf)
{
	if (!cd.present)
		return -1;

	uint8_t cdb[12] = {
		SCSI_READ10, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
		(uint8_t)(lba >> 8),  (uint8_t)lba,
		0,
		0, 1,                 /* transfer length: 1 logical block               */
		0, 0, 0,
	};

	int got = packet(cdb, buf, ATAPI_SECTOR_SIZE);
	if (got < 0)
		return -1;
	/* A short read on unit-attention (media just inserted) clears on retry. */
	if ((uint32_t)got < ATAPI_SECTOR_SIZE) {
		got = packet(cdb, buf, ATAPI_SECTOR_SIZE);
		if (got < (int)ATAPI_SECTOR_SIZE)
			return -1;
	}
	return 0;
}

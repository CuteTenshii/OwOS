/* hda.c — Intel High Definition Audio (HDA / "Azalia") PCM playback.
 *
 * An HDA *controller* (PCI class 0x04/0x03) is a PCI bus-master with a register
 * file in MMIO. It talks to one or more *codecs* over a serial link; the codec
 * is the thing with DACs, mixers and output pins. On real machines that codec
 * is almost always a Realtek ALC part, so this one driver is "Realtek audio".
 *
 * Bring-up (hda_init, runs before sti -> busy-waits, never sleeps):
 *   1. find the controller on PCI, map BAR0 uncached, enable bus-master DMA.
 *   2. reset the controller (GCTL.CRST) and wait for a codec on STATESTS.
 *   3. set up the CORB/RIRB command rings (how we send "verbs" to the codec).
 *   4. walk the codec's widget graph to find an output DAC -> pin path.
 *   5. configure that path (power, format, stream tag, unmute amps, EAPD).
 *
 * Playback (hda_play_wav, runs from the shell with interrupts on):
 *   resample the WAV to 48 kHz 16-bit stereo, stream it through one output
 *   stream descriptor over a cyclic 2-half DMA buffer, and pace the refills by
 *   *polling* the link position register (SDnLPIB) — no IRQ wiring, which keeps
 *   us off the PCI interrupt-sharing minefield on real hardware.
 *
 * Scope: enough of HDA for stereo PCM out. Long-form connection lists are
 * handled; multi-level mixer trees beyond one selector/mixer hop are not.
 */

#include <stdint.h>
#include "hda.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "timer.h"
#include "keyboard.h"
#include "console.h"
#include "fs.h"

/* ---- HDA controller MMIO register offsets ------------------------------ */
#define R_GCAP        0x00   /* 16: capabilities (stream counts)             */
#define R_GCTL        0x08   /* 32: bit0 CRST (controller reset)             */
#define R_STATESTS    0x0E   /* 16: one bit per codec link that responded    */
#define R_CORBLBASE   0x40
#define R_CORBUBASE   0x44
#define R_CORBWP      0x48   /* 16 */
#define R_CORBRP      0x4A   /* 16: bit15 = reset                            */
#define R_CORBCTL     0x4C   /* 8 : bit1 = run                               */
#define R_CORBSIZE    0x4E   /* 8 : low 2 bits = size code                   */
#define R_RIRBLBASE   0x50
#define R_RIRBUBASE   0x54
#define R_RIRBWP      0x58   /* 16: bit15 = reset                            */
#define R_RINTCNT     0x5A   /* 16 */
#define R_RIRBCTL     0x5C   /* 8 : bit1 = DMA enable                        */
#define R_RIRBSIZE    0x5E   /* 8 */

/* Stream descriptors begin at 0x80, 0x20 bytes apart. Offsets within one: */
#define SD_BASE       0x80
#define SD_STRIDE     0x20
#define SD_CTL        0x00   /* 24: b0 SRST, b1 RUN; bits20-23 stream number */
#define SD_LPIB       0x04   /* 32: link position in buffer                  */
#define SD_CBL        0x08   /* 32: cyclic buffer length (bytes)             */
#define SD_LVI        0x0C   /* 16: last valid BDL index                     */
#define SD_FMT        0x12   /* 16: stream format                            */
#define SD_BDPL       0x18   /* 32: BDL base low                             */
#define SD_BDPU       0x1C   /* 32: BDL base high                            */

/* ---- codec verbs / GET_PARAMETER ids ----------------------------------- */
#define GET_PARAM     0xF0000u            /* (0xF00 << 8)                    */
#define P_SUBNODE     0x04                /* subordinate node count          */
#define P_FGTYPE      0x05                /* function group type             */
#define P_AWCAP       0x09                /* audio widget capabilities       */
#define P_PINCAP      0x0C                /* pin capabilities                */
#define P_INAMPCAP    0x0D                /* input amp capabilities          */
#define P_CONNLEN     0x0E                /* connection list length          */
#define P_OUTAMPCAP   0x12                /* output amp capabilities         */

#define V_GET_CONN    0xF0200u            /* get connection list entry       */
#define V_GET_CFGDEF  0xF1C00u            /* get configuration default       */
#define V_SET_POWER   0x70500u            /* + state (D0 = 0)                 */
#define V_SET_FORMAT  0x20000u            /* + 16-bit format                 */
#define V_SET_STREAM  0x70600u            /* + (tag<<4)|channel              */
#define V_SET_CONNSEL 0x70100u            /* + index (selector)              */
#define V_SET_PINCTL  0x70700u            /* + control bits                  */
#define V_SET_EAPD    0x70C00u            /* + bit1 EAPD enable              */
#define V_SET_AMP     0x30000u            /* + 16-bit amp gain/mute payload  */

#define WT_DAC   0   /* audio output converter        */
#define WT_MIXER 2   /* mixer                         */
#define WT_SEL   3   /* selector                      */
#define WT_PIN   4   /* pin complex                   */

/* ---- driver state ------------------------------------------------------ */
static volatile uint8_t *mmio;
static int      present;
static uint8_t  codec;             /* codec address on the link             */
static uint16_t dac_nid, pin_nid, path_mix;  /* path_mix 0 = direct pin->DAC */
static int      mix_in_idx;        /* mixer/selector input index -> DAC      */
static int      pin_sel_idx;       /* pin input index -> mixer/DAC           */
static int      mix_is_sel;        /* path_mix is a selector (not a mixer)   */
static int      pin_is_hp;         /* output pin is a headphone jack         */
static uint32_t sd_off;            /* output stream descriptor offset        */

/* CORB/RIRB rings + the audio DMA buffer, all in one identity-mapped run. */
#define CORB_N   256
#define RIRB_N   256
#define AUDIO_BYTES 0x8000u        /* 32 KiB cyclic buffer                   */
#define HALF        (AUDIO_BYTES / 2)
static volatile uint32_t *corb;    /* CORB_N dwords                          */
static volatile uint32_t *rirb;    /* RIRB_N * 2 dwords (resp, resp_ex)      */
static volatile uint32_t *bdl;     /* buffer descriptor list                 */
static uint8_t           *audio;   /* the 32 KiB stereo S16 cyclic buffer    */
static uint32_t corb_phys, rirb_phys, bdl_phys;
static uint16_t corb_wp;

/* ---- MMIO accessors ---------------------------------------------------- */
static inline uint8_t  rd8 (uint32_t o)            { return *(volatile uint8_t  *)(mmio + o); }
static inline uint16_t rd16(uint32_t o)            { return *(volatile uint16_t *)(mmio + o); }
static inline uint32_t rd32(uint32_t o)            { return *(volatile uint32_t *)(mmio + o); }
static inline void     wr8 (uint32_t o, uint8_t v) { *(volatile uint8_t  *)(mmio + o) = v; }
static inline void     wr16(uint32_t o, uint16_t v){ *(volatile uint16_t *)(mmio + o) = v; }
static inline void     wr32(uint32_t o, uint32_t v){ *(volatile uint32_t *)(mmio + o) = v; }

static void spin(int n) { while (n-- > 0) __asm__ volatile ("pause"); }

/* ---- codec command over CORB/RIRB -------------------------------------- */
/* Push one command and poll the RIRB write pointer until the response lands.
 * One in flight at a time, so CORB index N <-> RIRB index N. Returns the 32-bit
 * response, or 0 on timeout. */
static uint32_t codec_cmd(uint16_t nid, uint32_t payload)
{
	uint32_t cmd = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) | (payload & 0xFFFFF);

	corb_wp = (uint16_t)((corb_wp + 1) & 0xFF);
	corb[corb_wp] = cmd;
	wr16(R_CORBWP, corb_wp);

	for (int t = 0; t < 100000; t++) {
		if ((rd16(R_RIRBWP) & 0xFF) == corb_wp)
			return rirb[corb_wp * 2];     /* response = first dword of entry  */
		spin(20);
	}
	return 0;
}

static uint32_t get_param(uint16_t nid, uint8_t p)
{
	return codec_cmd(nid, GET_PARAM | p);
}

/* One connection-list entry. Robust to codecs that round the requested index
 * down to a group boundary: fetch the aligned group, then pick our slot. The
 * list is "long form" (16-bit nids) when CONNLEN bit7 is set. */
static uint16_t conn_entry(uint16_t nid, int longform, int i)
{
	if (longform) {
		uint32_t r = codec_cmd(nid, V_GET_CONN | (uint32_t)(i & ~1));
		return (i & 1) ? (uint16_t)(r >> 16) : (uint16_t)(r & 0xFFFF);
	}
	uint32_t r = codec_cmd(nid, V_GET_CONN | (uint32_t)(i & ~3));
	return (uint16_t)((r >> ((i & 3) * 8)) & 0xFF);
}

static int widget_type(uint16_t nid) { return (int)((get_param(nid, P_AWCAP) >> 20) & 0xF); }

/* Set a widget's output amp to its loudest unmuted setting (no-op if it has no
 * output amp — the SET is simply ignored). bit15 set-output, 13/12 left/right,
 * bit7 mute (clear), 6-0 gain. */
static void amp_out_max(uint16_t nid)
{
	uint32_t cap  = get_param(nid, P_OUTAMPCAP);
	uint32_t gain = (cap >> 8) & 0x7F;     /* num-steps = loudest             */
	codec_cmd(nid, V_SET_AMP | 0xB000 | gain);
}

/* Unmute a widget's input amp at the given connection index (mixer/selector). */
static void amp_in_unmute(uint16_t nid, int idx)
{
	uint32_t cap  = get_param(nid, P_INAMPCAP);
	uint32_t gain = (cap >> 8) & 0x7F;
	codec_cmd(nid, V_SET_AMP | 0x7000 | ((uint32_t)idx << 8) | gain);
}

/* ---- codec graph walk -------------------------------------------------- */
/* Find an output DAC reachable from `pin`, either directly or through one
 * mixer/selector hop. Fills path_mix/mix_in_idx/pin_sel_idx/mix_is_sel. */
static int find_dac_path(uint16_t pin)
{
	uint32_t cl = get_param(pin, P_CONNLEN);
	int longf = (cl >> 7) & 1;
	int n = (int)(cl & 0x7F);

	for (int i = 0; i < n; i++) {                 /* direct pin -> DAC        */
		uint16_t src = conn_entry(pin, longf, i);
		if (src && widget_type(src) == WT_DAC) {
			dac_nid = src; path_mix = 0; pin_sel_idx = i;
			return 1;
		}
	}
	for (int i = 0; i < n; i++) {                 /* pin -> mixer/sel -> DAC  */
		uint16_t mid = conn_entry(pin, longf, i);
		if (!mid) continue;
		int wt = widget_type(mid);
		if (wt != WT_MIXER && wt != WT_SEL) continue;
		uint32_t mcl = get_param(mid, P_CONNLEN);
		int mlongf = (mcl >> 7) & 1, mn = (int)(mcl & 0x7F);
		for (int j = 0; j < mn; j++) {
			uint16_t src = conn_entry(mid, mlongf, j);
			if (src && widget_type(src) == WT_DAC) {
				dac_nid = src; path_mix = mid; mix_in_idx = j;
				mix_is_sel = (wt == WT_SEL); pin_sel_idx = i;
				return 1;
			}
		}
	}
	return 0;
}

/* Walk the audio function group, collect output-capable pins, and pick the best
 * one (speaker > line-out > headphone > anything) that reaches a DAC. */
static int enumerate_codec(void)
{
	uint32_t sub = get_param(0, P_SUBNODE);       /* root's function groups   */
	uint16_t fg_start = (uint16_t)((sub >> 16) & 0xFF);
	int      fg_count = (int)(sub & 0xFF);

	uint16_t afg = 0;
	for (int i = 0; i < fg_count; i++) {
		uint16_t fg = (uint16_t)(fg_start + i);
		if ((get_param(fg, P_FGTYPE) & 0xFF) == 0x01) { afg = fg; break; }
	}
	if (!afg)
		return 0;
	codec_cmd(afg, V_SET_POWER | 0x00);           /* power the AFG to D0      */

	uint32_t wsub = get_param(afg, P_SUBNODE);
	uint16_t w_start = (uint16_t)((wsub >> 16) & 0xFF);
	int      w_count = (int)(wsub & 0xFF);

	/* Rank candidate pins by default-device; lower rank wins. */
	uint16_t best_pin = 0; int best_rank = 99, best_hp = 0;
	for (int i = 0; i < w_count; i++) {
		uint16_t w = (uint16_t)(w_start + i);
		if (widget_type(w) != WT_PIN)
			continue;
		if (!((get_param(w, P_PINCAP) >> 4) & 1))         /* output-capable?  */
			continue;
		uint32_t cfg = codec_cmd(w, V_GET_CFGDEF);
		int conn = (int)((cfg >> 30) & 0x3);              /* 1 = no phys conn */
		int dev  = (int)((cfg >> 20) & 0xF);              /* default device   */
		if (conn == 1)
			continue;
		int rank;
		switch (dev) {
		case 0x1: rank = 0; break;   /* Speaker  */
		case 0x0: rank = 1; break;   /* Line Out */
		case 0x2: rank = 2; break;   /* HP Out   */
		default:  rank = 3; break;
		}
		if (rank < best_rank) {
			/* Only accept if a DAC path exists from this pin. */
			uint16_t save_dac = dac_nid; uint16_t save_mix = path_mix;
			if (find_dac_path(w)) {
				best_pin = w; best_rank = rank; best_hp = (dev == 0x2);
			} else {
				dac_nid = save_dac; path_mix = save_mix;
			}
		}
	}
	if (!best_pin)
		return 0;

	/* Re-resolve the chosen pin's path so the path_* globals match it. */
	pin_nid = best_pin; pin_is_hp = best_hp;
	return find_dac_path(pin_nid);
}

/* ---- bring-up ---------------------------------------------------------- */
static int find_controller(uint8_t *bus, uint8_t *slot, uint8_t *func)
{
	for (int b = 0; b < 256; b++)
		for (int s = 0; s < 32; s++) {
			if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
				continue;
			for (int f = 0; f < 8; f++) {
				if ((pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00) & 0xFFFF) == 0xFFFF)
					continue;
				uint32_t cls = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x08);
				if (((cls >> 24) & 0xFF) == 0x04 && ((cls >> 16) & 0xFF) == 0x03) {
					*bus = (uint8_t)b; *slot = (uint8_t)s; *func = (uint8_t)f;
					return 1;
				}
			}
		}
	return 0;
}

static int ring_setup(void)
{
	/* Carve one contiguous run: CORB | RIRB | BDL | audio. pmm_alloc_contig
	 * only reserves frames (above the 16 MiB identity map), so map it into the
	 * kernel directory before touching it. Left write-back cached: x86 keeps
	 * bus-master DMA coherent with the cache, so the controller sees our writes
	 * and we get fast buffer fills. */
	uint32_t base = pmm_alloc_contig(0x10000);
	if (!base || paging_identity_map(base, 0x10000) != 0)
		return 0;
	corb  = (volatile uint32_t *)(base + 0x0000);   corb_phys = base + 0x0000;
	rirb  = (volatile uint32_t *)(base + 0x0400);   rirb_phys = base + 0x0400;
	bdl   = (volatile uint32_t *)(base + 0x0C00);   bdl_phys  = base + 0x0C00;
	audio = (uint8_t *)         (base + 0x1000);

	wr8(R_CORBCTL, 0); wr8(R_RIRBCTL, 0);           /* stop the DMA engines    */

	wr32(R_CORBLBASE, corb_phys); wr32(R_CORBUBASE, 0);
	wr32(R_RIRBLBASE, rirb_phys); wr32(R_RIRBUBASE, 0);
	wr8(R_CORBSIZE, (rd8(R_CORBSIZE) & 0xFC) | 0x2);   /* 256 entries          */
	wr8(R_RIRBSIZE, (rd8(R_RIRBSIZE) & 0xFC) | 0x2);

	wr16(R_CORBRP, 0x8000);                            /* reset read pointer   */
	for (int t = 0; t < 1000; t++) { if (rd16(R_CORBRP) & 0x8000) break; spin(50); }
	wr16(R_CORBRP, 0x0000);
	for (int t = 0; t < 1000; t++) { if (!(rd16(R_CORBRP) & 0x8000)) break; spin(50); }
	wr16(R_CORBWP, 0x0000);
	corb_wp = 0;

	wr16(R_RIRBWP, 0x8000);                            /* reset write pointer  */
	wr16(R_RINTCNT, 0xFF);

	wr8(R_CORBCTL, 0x02);                              /* CORBRUN              */
	wr8(R_RIRBCTL, 0x02);                              /* RIRBDMAEN            */
	return 1;
}

void hda_init(void)
{
	uint8_t bus, slot, func;
	if (!find_controller(&bus, &slot, &func)) {
		con_write("hda: no HD Audio controller found\n");
		return;
	}

	uint32_t bar0 = pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0u;
	if (!bar0 || bar0 == 0xFFFFFFF0u) {
		con_write("hda: controller has no MMIO base\n");
		return;
	}
	/* memory-space decode (bit1) + bus-master DMA (bit2). */
	pci_write32(bus, slot, func, 0x04, pci_read32(bus, slot, func, 0x04) | 0x06);

	if (paging_identity_map(bar0, 0x4000) != 0) {
		con_write("hda: could not map MMIO\n");
		return;
	}
	paging_mark_uc(bar0, 0x4000);
	mmio = (volatile uint8_t *)bar0;

	/* Controller reset: CRST low, then high, then wait for it to come up. */
	wr32(R_GCTL, rd32(R_GCTL) & ~1u);
	for (int t = 0; t < 1000; t++) { if (!(rd32(R_GCTL) & 1)) break; spin(50); }
	wr32(R_GCTL, rd32(R_GCTL) | 1u);
	for (int t = 0; t < 1000; t++) { if (rd32(R_GCTL) & 1) break; spin(50); }

	/* Wait for a codec to report in on STATESTS (>= 521 us after reset). */
	uint16_t sts = 0;
	for (int t = 0; t < 2000 && !sts; t++) { sts = rd16(R_STATESTS); spin(200); }
	if (!sts) {
		con_write("hda: controller up but no codec responded\n");
		return;
	}
	for (codec = 0; codec < 15; codec++)
		if (sts & (1u << codec))
			break;

	if (!ring_setup()) {
		con_write("hda: ring buffer alloc failed\n");
		return;
	}

	if (!enumerate_codec()) {
		con_write("hda: no usable output path on codec\n");
		return;
	}

	/* The first output stream descriptor sits after the input descriptors. */
	uint16_t gcap = rd16(R_GCAP);
	int num_iss = (gcap >> 8) & 0x0F;
	sd_off = SD_BASE + (uint32_t)num_iss * SD_STRIDE;

	present = 1;
	con_write("hda: HD Audio ready (codec ");
	con_write_dec(codec);
	con_write(", dac nid ");
	con_write_dec(dac_nid);
	con_write(", pin nid ");
	con_write_dec(pin_nid);
	con_write(")\n");
}

int hda_present(void) { return present; }

/* ---- path configuration + stream programming --------------------------- */
#define FMT_48K_S16_STEREO  0x0011   /* base48k, x1, 16-bit, 2ch              */
#define STREAM_TAG          1

static void configure_path(void)
{
	/* DAC: power up, set format, bind to the stream, max output amp. */
	codec_cmd(dac_nid, V_SET_POWER  | 0x00);
	codec_cmd(dac_nid, V_SET_FORMAT | FMT_48K_S16_STEREO);
	codec_cmd(dac_nid, V_SET_STREAM | ((STREAM_TAG << 4) | 0));
	amp_out_max(dac_nid);

	if (path_mix) {
		codec_cmd(path_mix, V_SET_POWER | 0x00);
		if (mix_is_sel)
			codec_cmd(path_mix, V_SET_CONNSEL | (uint32_t)mix_in_idx);
		amp_in_unmute(path_mix, mix_in_idx);
		amp_out_max(path_mix);
	}

	/* Pin: power up, enable output (+ HP amp for a headphone), EAPD, amp. */
	codec_cmd(pin_nid, V_SET_POWER | 0x00);
	codec_cmd(pin_nid, V_SET_PINCTL | (pin_is_hp ? 0xC0 : 0x40));
	codec_cmd(pin_nid, V_SET_EAPD   | 0x02);
	amp_out_max(pin_nid);
}

/* ---- WAV parse + 48 kHz/S16/stereo resampler --------------------------- */
static uint32_t rd_le32(const uint8_t *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int tag_eq(const uint8_t *p, const char *s)
{ return p[0]==s[0] && p[1]==s[1] && p[2]==s[2] && p[3]==s[3]; }

/* Resampler / input-stream state (one playback at a time). */
static const char *r_fn;
static uint32_t r_data_off, r_data_len, r_filepos;
static uint16_t r_bits, r_channels;
static uint32_t r_frame, r_step;          /* r_step: 16.16 in-frames/out-frame */
static uint64_t r_outcount, r_inconsumed;
static int16_t  r_curL, r_curR;
static int      r_eof;
static uint8_t  r_inbuf[8192];
static uint32_t r_inlen, r_inpos;

/* Decode and consume the next input frame into r_curL/r_curR; sets r_eof and
 * yields silence once the data chunk is exhausted. */
static void decode_next(void)
{
	if (r_eof) { r_curL = r_curR = 0; return; }

	if (r_inpos + r_frame > r_inlen) {            /* refill the input buffer  */
		if (r_filepos >= r_data_len) { r_eof = 1; r_curL = r_curR = 0; return; }
		uint32_t want = r_data_len - r_filepos;
		if (want > sizeof(r_inbuf)) want = sizeof(r_inbuf);
		want -= want % r_frame;
		if (want == 0) want = r_frame;
		int got = fs_read_at(r_fn, r_data_off + r_filepos, r_inbuf, want);
		if (got <= 0 || (uint32_t)got < r_frame) { r_eof = 1; r_curL = r_curR = 0; return; }
		r_inlen = (uint32_t)got; r_inpos = 0; r_filepos += (uint32_t)got;
	}

	const uint8_t *s = r_inbuf + r_inpos;
	r_inpos += r_frame;
	if (r_bits == 8) {
		int l = ((int)s[0] - 128) << 8;
		r_curL = (int16_t)l;
		r_curR = (int16_t)((r_channels == 2) ? (((int)s[1] - 128) << 8) : l);
	} else {
		r_curL = (int16_t)rd_le16(s);
		r_curR = (r_channels == 2) ? (int16_t)rd_le16(s + 2) : r_curL;
	}
	r_inconsumed++;
}

/* Fill one HALF of the cyclic buffer with resampled 48 kHz S16 stereo frames.
 * Returns the number of frames that carried real audio (0 once fully drained). */
static uint32_t fill_half(int half)
{
	int16_t *dst = (int16_t *)(audio + (uint32_t)half * HALF);
	uint32_t frames = HALF / 4;
	uint32_t real = 0;
	for (uint32_t i = 0; i < frames; i++) {
		uint64_t target = (r_outcount * r_step) >> 16;
		while (r_inconsumed <= target && !r_eof)
			decode_next();
		dst[i * 2 + 0] = r_curL;
		dst[i * 2 + 1] = r_curR;
		if (!r_eof) real++;
		r_outcount++;
	}
	return real;
}

int hda_play_wav(const char *fn)
{
	if (!present) {
		con_write("play: no HD Audio output (start QEMU with -device intel-hda -device hda-output)\n");
		return -1;
	}

	int fsize = fs_size(fn);
	if (fsize < 0) { con_write("play: no such file: "); con_write(fn); con_putc('\n'); return -1; }

	uint8_t hdr[256];
	int got = fs_read_at(fn, 0, hdr, sizeof(hdr));
	if (got < 44 || !tag_eq(hdr, "RIFF") || !tag_eq(hdr + 8, "WAVE")) {
		con_write("play: not a WAV file\n");
		return -1;
	}

	uint16_t fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0, data_off = 0, data_len = 0;
	for (uint32_t pos = 12; pos + 8 <= (uint32_t)got; ) {
		const uint8_t *c = hdr + pos;
		uint32_t sz = rd_le32(c + 4);
		if (tag_eq(c, "fmt ")) {
			fmt = rd_le16(c + 8); channels = rd_le16(c + 10);
			rate = rd_le32(c + 12); bits = rd_le16(c + 22);
		} else if (tag_eq(c, "data")) {
			data_off = pos + 8; data_len = sz; break;
		}
		pos += 8 + sz + (sz & 1);
	}
	if (!data_off || fmt != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2) {
		con_write("play: unsupported WAV (need PCM 8/16-bit mono/stereo)\n");
		return -1;
	}
	if (data_off + data_len > (uint32_t)fsize)
		data_len = (uint32_t)fsize - data_off;
	if (rate < 4000)   rate = 4000;
	if (rate > 192000) rate = 192000;

	/* Init the resampler/stream state. */
	r_fn = fn; r_data_off = data_off; r_data_len = data_len; r_filepos = 0;
	r_bits = bits; r_channels = channels; r_frame = (uint32_t)channels * (bits / 8);
	r_step = (uint32_t)(((uint64_t)rate << 16) / 48000u);
	r_outcount = r_inconsumed = 0; r_curL = r_curR = 0; r_eof = 0;
	r_inlen = r_inpos = 0;

	configure_path();

	/* Program the output stream descriptor: reset, point at the BDL + buffer. */
	wr8(sd_off + SD_CTL, 0);                                   /* stop          */
	wr8(sd_off + SD_CTL, 0x01);                                /* SRST          */
	for (int t = 0; t < 1000; t++) { if (rd8(sd_off + SD_CTL) & 1) break; spin(50); }
	wr8(sd_off + SD_CTL, 0x00);
	for (int t = 0; t < 1000; t++) { if (!(rd8(sd_off + SD_CTL) & 1)) break; spin(50); }

	/* Two BDL entries, one per half of the cyclic buffer. */
	bdl[0] = (uint32_t)audio;             bdl[1] = 0; bdl[2] = HALF; bdl[3] = 0;
	bdl[4] = (uint32_t)audio + HALF;      bdl[5] = 0; bdl[6] = HALF; bdl[7] = 0;
	wr32(sd_off + SD_BDPL, bdl_phys); wr32(sd_off + SD_BDPU, 0);
	wr32(sd_off + SD_CBL, AUDIO_BYTES);
	wr16(sd_off + SD_LVI, 1);
	wr16(sd_off + SD_FMT, FMT_48K_S16_STEREO);
	wr8(sd_off + SD_CTL + 2, (uint8_t)(STREAM_TAG << 4));      /* stream number */

	keyboard_take_interrupt();            /* drop any stale Ctrl+C             */
	fill_half(0);
	fill_half(1);

	wr8(sd_off + SD_CTL, (uint8_t)(rd8(sd_off + SD_CTL) | 0x02));   /* RUN      */

	int interrupted = 0, last_half = 0, drain = 0, stalls = 0;
	uint32_t lastpos = 0;
	for (;;) {
		if (keyboard_take_interrupt()) { interrupted = 1; break; }

		uint32_t pos = rd32(sd_off + SD_LPIB);
		int cur_half = (pos >= HALF) ? 1 : 0;

		if (cur_half != last_half) {                  /* a half just finished */
			uint32_t real = fill_half(last_half);
			last_half = cur_half;
			if (real == 0) { if (++drain >= 2) break; }
			else drain = 0;
		}

		if (pos == lastpos) { if (++stalls > 500) break; }   /* DMA wedged    */
		else stalls = 0;
		lastpos = pos;

		timer_sleep(2);
	}

	wr8(sd_off + SD_CTL, 0);               /* stop the stream                  */
	return interrupted ? 1 : 0;
}

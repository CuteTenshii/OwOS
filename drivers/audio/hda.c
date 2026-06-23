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
 * Playback (hda_stream_*, fed from user space with interrupts on):
 *   /bin/play parses the WAV and resamples it to 48 kHz 16-bit stereo,
 *   then pushes ready frames here; this driver streams them through one output
 *   stream descriptor over a cyclic 2-half DMA buffer, pacing the producer by
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
#include "irqflags.h"

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
#define P_GPIOCNT     0x11                /* GPIO count (bidir/out/in fields)*/

#define V_GET_CONN    0xF0200u            /* get connection list entry       */
#define V_GET_CFGDEF  0xF1C00u            /* get configuration default       */
#define V_SET_POWER   0x70500u            /* + state (D0 = 0)                 */
#define V_SET_FORMAT  0x20000u            /* + 16-bit format                 */
#define V_SET_STREAM  0x70600u            /* + (tag<<4)|channel              */
#define V_SET_CONNSEL 0x70100u            /* + index (selector)              */
#define V_SET_PINCTL  0x70700u            /* + control bits                  */
#define V_SET_EAPD    0x70C00u            /* + bit1 EAPD enable              */
#define V_SET_AMP     0x30000u            /* + 16-bit amp gain/mute payload  */
#define V_SET_GPIODAT 0x71500u            /* + 8-bit GPIO data               */
#define V_SET_GPIOMSK 0x71600u            /* + 8-bit GPIO enable mask        */
#define V_SET_GPIODIR 0x71700u            /* + 8-bit GPIO direction (1=out)  */

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

/* Master volume: applied to one output-amp widget in the path (pin preferred,
 * else DAC, else mixer) so attenuation isn't multiplied across stages. */
static uint16_t vol_nid;           /* widget carrying the volume amp (0 = none) */
static uint32_t vol_steps;         /* its loudest gain value                  */
static int      hda_vol = HDA_VOL_MAX;
static int      hda_muted;

/* CORB/RIRB rings + the audio DMA buffer, all in one identity-mapped run. */
#define CORB_N   256
#define RIRB_N   256
#define AUDIO_BYTES 0x30000u       /* 192 KiB cyclic buffer (~1 s at 48k/S16/2ch):
                                    * deep enough that a transient producer stall
                                    * (a slow file read) can't let the play head
                                    * lap the writer and replay stale samples.   */
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

	/* Atomic against the keyboard IRQ, which can call in via hda_volume_*: the
	 * ring-pointer update + poll must not interleave with another command. Mask
	 * interrupts for the (microsecond) duration and restore the prior IF. */
	uintptr_t flags = irq_save();

	corb_wp = (uint16_t)((corb_wp + 1) & 0xFF);
	corb[corb_wp] = cmd;
	wr16(R_CORBWP, corb_wp);

	uint32_t resp = 0;
	for (int t = 0; t < 100000; t++) {
		if ((rd16(R_RIRBWP) & 0xFF) == corb_wp) {
			resp = rirb[corb_wp * 2];     /* response = first dword of entry  */
			break;
		}
		spin(20);
	}

	if (flags & 0x200)
		__asm__ volatile ("sti");
	return resp;
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

/* Output-amp num-steps (loudest gain value) a widget supports; 0 = no amp. */
static uint32_t out_steps(uint16_t nid) { return (get_param(nid, P_OUTAMPCAP) >> 8) & 0x7F; }

/* Set a widget's output amp, both channels, to `gain` steps and mute state.
 * bit15 set-output, 13/12 left/right, bit7 mute, 6-0 gain. No-op on a widget
 * with no output amp (the SET is simply ignored). */
static void set_out_amp(uint16_t nid, uint32_t gain, int mute)
{
	codec_cmd(nid, V_SET_AMP | 0xB000 | (mute ? 0x80 : 0) | (gain & 0x7F));
}

static void amp_out_max(uint16_t nid) { set_out_amp(nid, out_steps(nid), 0); }

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

/* Some real codecs (notably Realtek laptop parts) gate the internal speaker
 * amplifier and/or EAPD behind codec GPIO pins. QEMU's codec exposes none, so
 * this is invisible in emulation, and without a per-model quirk table there's no
 * telling which GPIO drives the amp — so assert every GPIO the codec advertises
 * as an output driven high, the common polarity for an amp/EAPD enable. No-ops
 * on a codec with no GPIOs, so it cannot regress the emulated path. */
static void enable_codec_gpios(uint16_t afg)
{
	uint32_t ngpio = get_param(afg, P_GPIOCNT) & 0xFF;   /* bidirectional pins */
	if (ngpio == 0)
		return;
	uint32_t mask = (ngpio >= 8) ? 0xFFu : ((1u << ngpio) - 1u);
	codec_cmd(afg, V_SET_GPIOMSK | mask);                /* enable these GPIOs */
	codec_cmd(afg, V_SET_GPIODIR | mask);                /* drive them as output */
	codec_cmd(afg, V_SET_GPIODAT | mask);                /* set them high        */
}

/* ---- output endpoint table --------------------------------------------- */
/* The codec usually exposes several output pins (speaker, headphone, line-out).
 * Keep them all so the active one can be switched at runtime, instead of
 * collapsing to a single best pin at init. Each entry caches the full resolved
 * DAC path for its pin. */
#define MAX_OUTPUTS 6
struct out_ep {
	uint16_t pin;          /* the output pin widget                          */
	uint16_t dac;          /* DAC reached from it                            */
	uint16_t mix;          /* mixer/selector hop, 0 = direct                 */
	int      mix_in_idx;   /* mixer/selector input index -> DAC              */
	int      pin_sel_idx;  /* pin input index -> mixer/DAC                   */
	int      mix_is_sel;   /* the hop is a selector (not a mixer)            */
	int      is_hp;        /* the pin is a headphone jack                    */
	int      dev;          /* config-default device code (names the output)  */
};
static struct out_ep outs[MAX_OUTPUTS];
static int n_outs;
static int cur_out = -1;   /* active index into outs[] (-1 = none)           */

/* Default-device code -> rank (lower wins) and a human name. */
static int dev_rank(int dev)
{
	switch (dev) {
	case 0x1: return 0;   /* Speaker  */
	case 0x0: return 1;   /* Line Out */
	case 0x2: return 2;   /* HP Out   */
	default:  return 3;
	}
}
static const char *dev_name(int dev)
{
	switch (dev) {
	case 0x1: return "Speakers";
	case 0x0: return "Line Out";
	case 0x2: return "Headphones";
	default:  return "Digital Out";
	}
}

/* Choose the single widget the volume keys/amp drive for the active path
 * (pin > DAC > mixer), so attenuation isn't multiplied across stages. */
static void pick_vol_widget(void)
{
	vol_nid = 0; vol_steps = 0;
	uint16_t cand[3]; int nc = 0;
	cand[nc++] = pin_nid;
	cand[nc++] = dac_nid;
	if (path_mix) cand[nc++] = path_mix;
	for (int i = 0; i < nc; i++) {
		uint32_t st = out_steps(cand[i]);
		if (st) { vol_nid = cand[i]; vol_steps = st; break; }
	}
}

/* Copy the active endpoint into the path globals and re-pick its volume widget,
 * so configure_path()/the volume keys act on the selected output. */
static void apply_active_globals(void)
{
	struct out_ep *e = &outs[cur_out];
	dac_nid    = e->dac;        pin_nid     = e->pin;     path_mix  = e->mix;
	mix_in_idx = e->mix_in_idx; pin_sel_idx = e->pin_sel_idx;
	mix_is_sel = e->mix_is_sel; pin_is_hp   = e->is_hp;
	pick_vol_widget();
}

/* Collect every output-capable pin that reaches a DAC into outs[], ranked
 * speaker > line-out > headphone > other. A strict pass first; only if it finds
 * nothing, a fallback pass also trusts pins whose config default mislabels a
 * wired output as "no physical connection". Returns the count. */
static int collect_outputs(uint16_t w_start, int w_count)
{
	n_outs = 0;
	for (int pass = 0; pass < 2 && n_outs == 0; pass++) {
		for (int i = 0; i < w_count && n_outs < MAX_OUTPUTS; i++) {
			uint16_t w = (uint16_t)(w_start + i);
			if (widget_type(w) != WT_PIN)
				continue;
			if (!((get_param(w, P_PINCAP) >> 4) & 1))      /* output-capable?  */
				continue;
			uint32_t cfg = codec_cmd(w, V_GET_CFGDEF);
			int conn = (int)((cfg >> 30) & 0x3);           /* 1 = no phys conn */
			int dev  = (int)((cfg >> 20) & 0xF);           /* default device   */
			if (conn == 1 && pass == 0)
				continue;
			uint16_t save_dac = dac_nid, save_mix = path_mix;
			if (!find_dac_path(w)) { dac_nid = save_dac; path_mix = save_mix; continue; }
			struct out_ep *e = &outs[n_outs++];
			e->pin = w;                 e->dac = dac_nid;          e->mix = path_mix;
			e->mix_in_idx = mix_in_idx; e->pin_sel_idx = pin_sel_idx;
			e->mix_is_sel = mix_is_sel; e->is_hp = (dev == 0x2);   e->dev = dev;
		}
	}
	/* Insertion-sort by rank so the speaker is the default active output. */
	for (int i = 1; i < n_outs; i++) {
		struct out_ep tmp = outs[i];
		int j = i - 1;
		while (j >= 0 && dev_rank(outs[j].dev) > dev_rank(tmp.dev)) {
			outs[j + 1] = outs[j]; j--;
		}
		outs[j + 1] = tmp;
	}
	return n_outs;
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
	enable_codec_gpios(afg);                      /* unmute a GPIO-gated amp  */

	uint32_t wsub = get_param(afg, P_SUBNODE);
	uint16_t w_start = (uint16_t)((wsub >> 16) & 0xFF);
	int      w_count = (int)(wsub & 0xFF);

	/* Collect every switchable output pin (speaker/headphones/line-out/...),
	 * then make the best-ranked one active so the volume keys and the first play
	 * route correctly before anything is selected. */
	if (!collect_outputs(w_start, w_count))
		return 0;
	cur_out = 0;
	apply_active_globals();
	return 1;
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
	uint32_t base = pmm_alloc_contig(0x40000);
	if (!base || paging_identity_map(base, 0x40000) != 0)
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

/* Push hda_vol/hda_muted onto the volume widget's output amp. Scales the 0..MAX
 * level onto 0..num-steps; mute drops the gain to 0 and sets the mute bit. */
static void hda_apply_volume(void)
{
	if (!present || !vol_nid || !vol_steps)
		return;
	uint32_t gain = hda_muted ? 0 : ((uint32_t)hda_vol * vol_steps / HDA_VOL_MAX);
	set_out_amp(vol_nid, gain, hda_muted);
}

void hda_volume_step(int dir)
{
	int nv = hda_vol + dir;
	if (nv < 0)           nv = 0;
	if (nv > HDA_VOL_MAX) nv = HDA_VOL_MAX;
	hda_vol = nv;
	hda_muted = 0;                 /* nudging the level always unmutes         */
	hda_apply_volume();
}

void hda_volume_mute(void)
{
	hda_muted = !hda_muted;
	hda_apply_volume();
}

int hda_volume(void)
{
	return hda_muted ? 0 : hda_vol;
}

static void configure_path(void)
{
	apply_active_globals();          /* sync globals + vol widget to cur_out      */

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

	/* Disable every other output pin so playback is exclusive to the selected
	 * one — speaker and headphone often share a DAC, so leaving both pins enabled
	 * would drive both jacks at once. */
	for (int i = 0; i < n_outs; i++) {
		if (i == cur_out)
			continue;
		codec_cmd(outs[i].pin, V_SET_PINCTL | 0x00);   /* output disable */
		set_out_amp(outs[i].pin, 0, 1);                /* mute its amp   */
	}

	/* Active pin: power up, enable output (+ HP amp for a headphone), EAPD, amp. */
	codec_cmd(pin_nid, V_SET_POWER | 0x00);
	codec_cmd(pin_nid, V_SET_PINCTL | (pin_is_hp ? 0xC0 : 0x40));
	codec_cmd(pin_nid, V_SET_EAPD   | 0x02);
	amp_out_max(pin_nid);

	/* Override the volume widget's amp with the current level (the keys may have
	 * moved it before this play). */
	hda_apply_volume();
}

/* ---- streaming PCM sink (48 kHz / S16 / stereo, fed from user space) --------
 *
 * The WAV parse + resample now live in /bin/play: it streams the file
 * in small chunks, resamples to this canonical format, and pushes ready frames
 * here over the SYS_AUDIO_* syscalls. This driver is just the DMA back end —
 * a cyclic buffer the producer fills behind the play head. Pushing tiny bounded
 * chunks (rather than running the whole multi-minute playback inside one kernel
 * call) is what keeps the machine responsive while playing off slow/flaky media.
 *
 * One stream at a time. The producer writes into the region the DMA has already
 * played, tracked by the link-position register (SDnLPIB); hda_stream_write
 * back-pressures (timer_sleep) until the DMA frees space, so it self-paces to
 * the 48 kHz output rate without an IRQ. */
#define STREAM_GUARD 256u           /* bytes kept clear ahead of the play head  */

static int      s_active;           /* a stream is open                         */
static int      s_started;          /* RUN asserted (buffer pre-rolled)         */
static uint32_t s_wpos;             /* next write offset into audio[] (bytes)   */

/* Reset + program the output stream descriptor for the cyclic 2-half buffer. */
static void stream_program_sd(void)
{
	wr8(sd_off + SD_CTL, 0);                                   /* stop          */
	wr8(sd_off + SD_CTL, 0x01);                                /* SRST          */
	for (int t = 0; t < 1000; t++) { if (rd8(sd_off + SD_CTL) & 1) break; spin(50); }
	wr8(sd_off + SD_CTL, 0x00);
	for (int t = 0; t < 1000; t++) { if (!(rd8(sd_off + SD_CTL) & 1)) break; spin(50); }

	bdl[0] = (uint32_t)audio;        bdl[1] = 0; bdl[2] = HALF; bdl[3] = 0;
	bdl[4] = (uint32_t)audio + HALF; bdl[5] = 0; bdl[6] = HALF; bdl[7] = 0;
	wr32(sd_off + SD_BDPL, bdl_phys); wr32(sd_off + SD_BDPU, 0);
	wr32(sd_off + SD_CBL, AUDIO_BYTES);
	wr16(sd_off + SD_LVI, 1);
	wr16(sd_off + SD_FMT, FMT_48K_S16_STEREO);
	wr8(sd_off + SD_CTL + 2, (uint8_t)(STREAM_TAG << 4));      /* stream number */
}

/* Open the stream: configure the codec path and the stream descriptor, clear
 * the buffer to silence, and arm the producer. Returns 0, or -1 if no codec. */
int hda_stream_open(void)
{
	if (!present)
		return -1;
	configure_path();
	stream_program_sd();
	for (uint32_t i = 0; i < AUDIO_BYTES; i++)
		audio[i] = 0;
	s_active = 1;
	s_started = 0;
	s_wpos = 0;
	keyboard_take_interrupt();            /* drop any stale Ctrl+C              */
	return 0;
}

/* Push `bytes` of 48 kHz/S16/stereo PCM, blocking until it is queued. Returns
 * the count consumed (== bytes), -1 if no stream is open, or -2 if Ctrl+C was
 * pressed (so /bin/play stops). Frame-aligned (4-byte) writes are assumed. */
int hda_stream_write(const void *pcm, uint32_t bytes)
{
	if (!s_active)
		return -1;
	const uint8_t *p = pcm;
	uint32_t off = 0;
	while (off < bytes) {
		if (keyboard_take_interrupt())
			return -2;

		if (!s_started) {
			/* Pre-roll: fill the buffer linearly; start RUN once it is full. A
			 * clip shorter than the buffer simply never starts here — close()
			 * pads it and plays it out. */
			uint32_t space = AUDIO_BYTES - s_wpos;
			uint32_t n = bytes - off;
			if (n > space)
				n = space;
			for (uint32_t i = 0; i < n; i++)
				audio[s_wpos + i] = p[off + i];
			s_wpos += n;
			off += n;
			if (s_wpos >= AUDIO_BYTES) {
				wr8(sd_off + SD_CTL, (uint8_t)(rd8(sd_off + SD_CTL) | 0x02));
				s_started = 1;
				s_wpos = 0;
			}
			continue;
		}

		/* Steady state: write into the bytes the play head has already passed. */
		uint32_t rpos = rd32(sd_off + SD_LPIB);
		uint32_t freebytes = (rpos + AUDIO_BYTES - s_wpos) % AUDIO_BYTES;
		if (freebytes <= STREAM_GUARD) {      /* DMA hasn't freed room yet       */
			timer_sleep(2);
			continue;
		}
		uint32_t can = (freebytes - STREAM_GUARD) & ~3u;   /* frame-aligned      */
		if (can == 0) {
			timer_sleep(2);
			continue;
		}
		uint32_t to_end = AUDIO_BYTES - s_wpos;
		uint32_t n = bytes - off;
		if (n > can)    n = can;
		if (n > to_end) n = to_end;
		for (uint32_t i = 0; i < n; i++)
			audio[s_wpos + i] = p[off + i];
		s_wpos += n;
		if (s_wpos >= AUDIO_BYTES)
			s_wpos -= AUDIO_BYTES;
		off += n;
	}
	return (int)off;
}

/* Drain whatever is still queued, then stop the stream. */
void hda_stream_close(void)
{
	if (!s_active)
		return;

	if (!s_started) {
		/* Clip shorter than the buffer: pad the tail with silence, play it once. */
		uint32_t real = s_wpos;
		for (uint32_t i = s_wpos; i < AUDIO_BYTES; i++)
			audio[i] = 0;
		wr8(sd_off + SD_CTL, (uint8_t)(rd8(sd_off + SD_CTL) | 0x02));
		s_started = 1;
		timer_sleep(real / 192 + 30);     /* ~192 bytes/ms at 48 kHz/4B + tail   */
	} else {
		/* Push a full buffer of silence behind the play head (back-pressured),
		 * which proves the DMA has consumed every real sample, then let the
		 * silence flush. */
		uint32_t silence = AUDIO_BYTES;
		int guard = 0;
		while (silence > 0 && guard < 4000) {
			if (keyboard_take_interrupt())
				break;
			uint32_t rpos = rd32(sd_off + SD_LPIB);
			uint32_t freebytes = (rpos + AUDIO_BYTES - s_wpos) % AUDIO_BYTES;
			if (freebytes <= STREAM_GUARD) { timer_sleep(2); guard++; continue; }
			uint32_t can = (freebytes - STREAM_GUARD) & ~3u;
			if (can == 0) { timer_sleep(2); guard++; continue; }
			uint32_t to_end = AUDIO_BYTES - s_wpos;
			uint32_t n = silence;
			if (n > can)    n = can;
			if (n > to_end) n = to_end;
			for (uint32_t i = 0; i < n; i++)
				audio[s_wpos + i] = 0;
			s_wpos += n;
			if (s_wpos >= AUDIO_BYTES)
				s_wpos -= AUDIO_BYTES;
			silence -= n;
		}
		timer_sleep(180);                  /* let the last queued audio play out */
	}

	wr8(sd_off + SD_CTL, 0);               /* stop the stream                   */
	s_active = 0;
	s_started = 0;
}

/* ---- output endpoint switching (Sound settings + /sys/class/sound) ------ *
 * The codec's output pins are enumerated once at init; these expose them so the
 * Sound panel and /sys can list them and switch the active output at runtime.
 * All degrade to empty/-1/failure when no codec is present. */
int hda_output_count(void) { return present ? n_outs : 0; }

int hda_output_name(int i, char *out, int max)
{
	if (!present || i < 0 || i >= n_outs || max <= 0)
		return 0;
	const char *s = dev_name(outs[i].dev);
	int k = 0;
	for (; s[k] && k < max - 1; k++) out[k] = s[k];
	out[k] = 0;
	return 1;
}

int hda_output_active(void) { return present ? cur_out : -1; }

int hda_output_select(int i)
{
	if (!present || i < 0 || i >= n_outs)
		return -1;
	cur_out = i;
	/* Re-route a live stream immediately; otherwise just record the choice so the
	 * next play opens on the newly selected output. */
	if (s_active)
		configure_path();
	else
		apply_active_globals();
	return 0;
}


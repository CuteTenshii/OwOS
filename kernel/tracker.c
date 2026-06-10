/* tracker.c — HTTP tracker announce + peer-list parsing. See tracker.h. */

#include "tracker.h"
#include "bencode.h"
#include "net.h"
#include "heap.h"
#include "vga.h"

/* Our peer id: 20 bytes, Azureus-style client prefix + filler. Trackers treat
 * it as opaque; we never accept incoming connections, so it need not be
 * registered anywhere. */
static const char PEER_ID[] = "-OW0001-owos00000001";  /* 20 bytes used */

#define TRACKER_RXMAX 16384     /* tracker replies are small (compact peers)   */

/* Percent-encode `n` raw bytes into out (cap bytes incl. NUL), leaving the RFC
 * 3986 unreserved set untouched — exactly what info_hash/peer_id need (they
 * are raw bytes that may contain anything, including NUL). Returns the string
 * length, or -1 on overflow. */
static int pct_encode(const uint8_t *in, uint32_t n, char *out, int cap)
{
	static const char hx[] = "0123456789ABCDEF";
	int o = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = in[i];
		int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                 (c >= '0' && c <= '9') ||
		                 c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved) {
			if (o + 1 >= cap) return -1;
			out[o++] = (char)c;
		} else {
			if (o + 3 >= cap) return -1;
			out[o++] = '%';
			out[o++] = hx[c >> 4];
			out[o++] = hx[c & 0xf];
		}
	}
	if (o >= cap) return -1;
	out[o] = '\0';
	return o;
}

/* Append a NUL-terminated string to buf[*pos], bounded by cap. */
static int sappend(char *buf, int pos, int cap, const char *s)
{
	while (*s && pos < cap - 1)
		buf[pos++] = *s++;
	buf[pos] = '\0';
	return pos;
}

/* Append an unsigned 64-bit decimal. */
static int uappend(char *buf, int pos, int cap, uint64_t v)
{
	char tmp[20];
	int i = 0;
	if (v == 0) tmp[i++] = '0';
	while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i-- && pos < cap - 1)
		buf[pos++] = tmp[i];
	buf[pos] = '\0';
	return pos;
}

/* Build the full announce URL with its query string into `url` (cap bytes). A
 * tracker URL may already carry a query (e.g. a passkey), so the separator is
 * '&' if there's a '?' already, else '?'. Returns length or -1 on overflow. */
static int build_announce_url(char *url, int cap, const char *announce,
                              const uint8_t infohash[20], int64_t left)
{
	int has_q = 0;
	for (const char *p = announce; *p; p++)
		if (*p == '?') { has_q = 1; break; }

	int n = sappend(url, 0, cap, announce);
	n = sappend(url, n, cap, has_q ? "&info_hash=" : "?info_hash=");

	char enc[64];
	if (pct_encode(infohash, 20, enc, (int)sizeof(enc)) < 0) return -1;
	n = sappend(url, n, cap, enc);

	n = sappend(url, n, cap, "&peer_id=");
	if (pct_encode((const uint8_t *)PEER_ID, 20, enc, (int)sizeof(enc)) < 0) return -1;
	n = sappend(url, n, cap, enc);

	n = sappend(url, n, cap, "&port=6881&uploaded=0&downloaded=0&left=");
	n = uappend(url, n, cap, (uint64_t)(left < 0 ? 0 : left));
	n = sappend(url, n, cap, "&compact=1&event=started");

	if (n >= cap - 1)               /* sappend/uappend stop short, so a near-
	                                 * full buffer means we truncated */
		return -1;
	return n;
}

/* Extract peers from a parsed reply into peers[0..max). Handles both the
 * compact string form (6 bytes/peer: 4 IP + 2 port, big-endian) and the
 * legacy list-of-dicts form. Returns the count written. */
static int parse_peers(const struct bencode *b, int peers_node,
                       struct tracker_peer *peers, int max)
{
	int count = 0;
	const struct ben_node *pn = &b->nodes[peers_node];

	if (pn->type == BEN_STR) {                      /* compact (BEP 23)        */
		uint32_t k = 0;
		while (k + 6 <= pn->slen && count < max) {
			peers[count].ip[0] = pn->str[k];
			peers[count].ip[1] = pn->str[k + 1];
			peers[count].ip[2] = pn->str[k + 2];
			peers[count].ip[3] = pn->str[k + 3];
			peers[count].port  = (uint16_t)((pn->str[k + 4] << 8) |
			                                 pn->str[k + 5]);
			count++;
			k += 6;
		}
	} else if (pn->type == BEN_LIST) {              /* list of dicts           */
		int n = ben_list_count(b, peers_node);
		for (int i = 0; i < n && count < max; i++) {
			int d = ben_list_at(b, peers_node, i);
			int ip = ben_dict_get(b, d, "ip");
			int pt = ben_dict_get(b, d, "port");
			if (ip < 0 || pt < 0 ||
			    b->nodes[ip].type != BEN_STR ||
			    b->nodes[pt].type != BEN_INT)
				continue;
			/* parse dotted-quad "a.b.c.d" */
			const struct ben_node *s = &b->nodes[ip];
			uint32_t val = 0, parts = 0;
			int bad = 0;
			for (uint32_t j = 0; j <= s->slen; j++) {
				if (j == s->slen || s->str[j] == '.') {
					if (parts < 4) peers[count].ip[parts] = (uint8_t)val;
					parts++; val = 0;
				} else if (s->str[j] >= '0' && s->str[j] <= '9') {
					val = val * 10 + (s->str[j] - '0');
				} else { bad = 1; break; }
			}
			if (bad || parts != 4) continue;
			peers[count].port = (uint16_t)b->nodes[pt].ival;
			count++;
		}
	}
	return count;
}

int tracker_announce(const char *announce, const uint8_t infohash[20],
                     int64_t left, struct tracker_peer *peers, int max,
                     int *interval_out)
{
	if (!announce || announce[0] == '\0') {
		vga_write("tracker: torrent has no announce URL\n");
		return -1;
	}

	char url[640];
	if (build_announce_url(url, (int)sizeof(url), announce, infohash, left) < 0) {
		vga_write("tracker: announce URL too long\n");
		return -1;
	}

	uint8_t *rx = kmalloc(TRACKER_RXMAX);
	struct ben_node *pool = kmalloc(512 * sizeof *pool);
	if (!rx || !pool) {
		vga_write("tracker: out of memory\n");
		kfree(rx); kfree(pool);
		return -1;
	}

	int n = net_http_get(url, rx, TRACKER_RXMAX);
	if (n < 0) {
		vga_write("tracker: announce request failed\n");
		kfree(rx); kfree(pool);
		return -1;
	}

	struct bencode b;
	int rc = -1;
	if (ben_parse(&b, rx, (uint32_t)n, pool, 512) != 0 ||
	    b.nodes[b.root].type != BEN_DICT) {
		vga_write("tracker: malformed reply\n");
		goto out;
	}

	int fail = ben_dict_get(&b, b.root, "failure reason");
	if (fail >= 0 && b.nodes[fail].type == BEN_STR) {
		vga_write("tracker error: ");
		for (uint32_t i = 0; i < b.nodes[fail].slen; i++)
			vga_putc((char)b.nodes[fail].str[i]);
		vga_putc('\n');
		goto out;
	}

	int iv = ben_dict_get(&b, b.root, "interval");
	if (interval_out)
		*interval_out = (iv >= 0 && b.nodes[iv].type == BEN_INT)
		              ? (int)b.nodes[iv].ival : 0;

	int pp = ben_dict_get(&b, b.root, "peers");
	if (pp < 0) {
		vga_write("tracker: reply has no peers\n");
		goto out;
	}
	rc = parse_peers(&b, pp, peers, max);

out:
	kfree(rx); kfree(pool);
	return rc;
}

void tracker_print(const struct tracker_peer *peers, int count, int interval)
{
	vga_write("tracker: ");
	/* small decimal print via uappend into a scratch line */
	char line[32];
	int ln = uappend(line, 0, (int)sizeof(line), (uint64_t)count);
	line[ln] = '\0';
	vga_write(line);
	vga_write(count == 1 ? " peer" : " peers");
	if (interval > 0) {
		vga_write(", re-announce in ");
		ln = uappend(line, 0, (int)sizeof(line), (uint64_t)interval);
		line[ln] = '\0';
		vga_write(line);
		vga_write("s");
	}
	vga_putc('\n');

	for (int i = 0; i < count; i++) {
		vga_write("  ");
		for (int o = 0; o < 4; o++) {
			char num[4];
			int nn = uappend(num, 0, (int)sizeof(num), peers[i].ip[o]);
			num[nn] = '\0';
			vga_write(num);
			vga_putc(o < 3 ? '.' : ':');
		}
		char num[8];
		int nn = uappend(num, 0, (int)sizeof(num), peers[i].port);
		num[nn] = '\0';
		vga_write(num);
		vga_putc('\n');
	}
}

/* --------------------------------------------------------------- self-test */

/* A canned compact-peers reply: d8:intervali1800e5:peers12:<2 peers>e
 *   peer 0 = 1.2.3.4:6881   (0x1AE1)
 *   peer 1 = 5.6.7.8:51413  (0xC8D5)                                          */
static const uint8_t sample_reply[] = {
	'd','8',':','i','n','t','e','r','v','a','l','i','1','8','0','0','e',
	'5',':','p','e','e','r','s','1','2',':',
	0x01,0x02,0x03,0x04,0x1a,0xe1, 0x05,0x06,0x07,0x08,0xc8,0xd5,
	'e',
};

int tracker_selftest(void)
{
	int ok = 1;

	/* percent-encoding known answer: 4 bytes 00 1F 2D 41 -> "%00%1F-A"
	 * (2D '-' and 41 'A' are unreserved, 00 and 1F escape). */
	uint8_t raw[4] = { 0x00, 0x1f, 0x2d, 0x41 };
	char enc[16];
	int el = pct_encode(raw, 4, enc, (int)sizeof(enc));
	const char *want = "%00%1F-A";
	int pct_ok = (el == 8);
	for (int i = 0; want[i] && pct_ok; i++)
		if (enc[i] != want[i]) pct_ok = 0;
	vga_write("  pct-encode "); vga_write(pct_ok ? "PASS\n" : "FAIL\n");

	/* peer extraction from the canned reply */
	struct ben_node pool[64];
	struct bencode b;
	struct tracker_peer peers[8];
	int peers_ok = 0, iv = 0, cnt = -1;
	if (ben_parse(&b, sample_reply, (uint32_t)sizeof(sample_reply), pool, 64) == 0) {
		int iv_n = ben_dict_get(&b, b.root, "interval");
		iv = (iv_n >= 0) ? (int)b.nodes[iv_n].ival : 0;
		int pp = ben_dict_get(&b, b.root, "peers");
		cnt = (pp >= 0) ? parse_peers(&b, pp, peers, 8) : -1;
	}
	peers_ok = (cnt == 2 && iv == 1800 &&
	            peers[0].ip[0] == 1 && peers[0].ip[3] == 4 && peers[0].port == 6881 &&
	            peers[1].ip[0] == 5 && peers[1].ip[3] == 8 && peers[1].port == 51413);
	vga_write("  peers      "); vga_write(peers_ok ? "PASS\n" : "FAIL\n");

	ok = pct_ok && peers_ok;
	vga_write(ok ? "tracker selftest: PASS\n" : "tracker selftest: FAIL\n");
	return ok ? 0 : -1;
}

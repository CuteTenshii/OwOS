/* torrent.c — .torrent metainfo parsing + infohash. See torrent.h. */

#include "torrent.h"
#include "bencode.h"
#include "crypto.h"
#include "heap.h"
#include "vga.h"

#define TORRENT_MAX_NODES 8192  /* enough for a large multi-file torrent: each
                                 * file is a dict (~6 nodes), so ~1000 files   */

/* Copy a BEN_STR span into a bounded, NUL-terminated C buffer. */
static void str_to_buf(const struct ben_node *n, char *out, uint32_t cap)
{
	uint32_t i = 0, lim = (n->slen < cap - 1) ? n->slen : cap - 1;
	for (; i < lim; i++)
		out[i] = (char)n->str[i];
	out[i] = '\0';
}

/* Pull the primary tracker URL: the "announce" key, else the first entry of
 * "announce-list" (a list of lists of strings). Leaves announce_buf empty if
 * neither is present (e.g. a trackerless torrent). */
static void find_announce(struct torrent *t)
{
	t->announce_buf[0] = '\0';
	t->announce = t->announce_buf;

	int a = ben_dict_get(&t->ben, t->ben.root, "announce");
	if (a >= 0 && t->ben.nodes[a].type == BEN_STR) {
		str_to_buf(&t->ben.nodes[a], t->announce_buf,
		           (uint32_t)sizeof(t->announce_buf));
		return;
	}
	int al = ben_dict_get(&t->ben, t->ben.root, "announce-list");
	int tier = ben_list_at(&t->ben, al, 0);
	int url  = ben_list_at(&t->ben, tier, 0);
	if (url >= 0 && t->ben.nodes[url].type == BEN_STR)
		str_to_buf(&t->ben.nodes[url], t->announce_buf,
		           (uint32_t)sizeof(t->announce_buf));
}

int torrent_parse(struct torrent *t, const uint8_t *buf, uint32_t len)
{
	struct ben_node *pool = kmalloc(TORRENT_MAX_NODES * sizeof *pool);
	if (!pool)
		return -1;

	if (ben_parse(&t->ben, buf, len, pool, TORRENT_MAX_NODES) != 0 ||
	    t->ben.nodes[t->ben.root].type != BEN_DICT) {
		kfree(pool);
		return -1;
	}

	int info = ben_dict_get(&t->ben, t->ben.root, "info");
	if (info < 0 || t->ben.nodes[info].type != BEN_DICT)
		goto fail;

	/* The infohash is SHA-1 over the info dictionary's exact encoded bytes,
	 * captured verbatim by the parser — not a re-serialisation. */
	sha1(t->ben.nodes[info].raw, t->ben.nodes[info].rawlen, t->infohash);

	int pl = ben_dict_get(&t->ben, info, "piece length");
	int pc = ben_dict_get(&t->ben, info, "pieces");
	int nm = ben_dict_get(&t->ben, info, "name");
	if (pl < 0 || t->ben.nodes[pl].type != BEN_INT ||
	    pc < 0 || t->ben.nodes[pc].type != BEN_STR ||
	    nm < 0 || t->ben.nodes[nm].type != BEN_STR)
		goto fail;
	if (t->ben.nodes[pc].slen % 20 != 0)            /* hashes are 20 bytes each */
		goto fail;

	t->piece_length = t->ben.nodes[pl].ival;
	t->pieces   = t->ben.nodes[pc].str;
	t->n_pieces = t->ben.nodes[pc].slen / 20;
	t->name     = t->ben.nodes[nm].str;
	t->name_len = t->ben.nodes[nm].slen;

	/* Single-file ("length") vs multi-file ("files": list of {length, path}). */
	int single = ben_dict_get(&t->ben, info, "length");
	int files  = ben_dict_get(&t->ben, info, "files");
	if (single >= 0 && t->ben.nodes[single].type == BEN_INT) {
		t->multi_file   = 0;
		t->file_count   = 1;
		t->total_length = t->ben.nodes[single].ival;
	} else if (files >= 0 && t->ben.nodes[files].type == BEN_LIST) {
		t->multi_file   = 1;
		t->file_count   = ben_list_count(&t->ben, files);
		t->total_length = 0;
		for (int i = 0; i < t->file_count; i++) {
			int f = ben_list_at(&t->ben, files, i);
			int fl = ben_dict_get(&t->ben, f, "length");
			if (fl < 0 || t->ben.nodes[fl].type != BEN_INT)
				goto fail;
			t->total_length += t->ben.nodes[fl].ival;
		}
	} else {
		goto fail;
	}

	find_announce(t);
	return 0;

fail:
	kfree(pool);
	t->ben.nodes = (struct ben_node *)0;
	return -1;
}

void torrent_free(struct torrent *t)
{
	kfree(t->ben.nodes);
	t->ben.nodes = (struct ben_node *)0;
}

/* ----------------------------------------------------------------- printing */

static void print_u64(uint64_t v)
{
	char tmp[20];
	int i = 0;
	if (v == 0) { vga_putc('0'); return; }
	while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i--) vga_putc(tmp[i]);
}

static void print_hex20(const uint8_t h[20])
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 20; i++) {
		vga_putc(hx[h[i] >> 4]);
		vga_putc(hx[h[i] & 0xf]);
	}
}

void torrent_print(const struct torrent *t)
{
	vga_write("name:      ");
	for (uint32_t i = 0; i < t->name_len; i++)
		vga_putc((char)t->name[i]);
	vga_putc('\n');

	vga_write("infohash:  ");
	print_hex20(t->infohash);
	vga_putc('\n');

	vga_write("announce:  ");
	vga_write(t->announce[0] ? t->announce : "(none)");
	vga_putc('\n');

	vga_write("layout:    ");
	vga_write(t->multi_file ? "multi-file (" : "single-file (");
	print_u64((uint64_t)t->file_count);
	vga_write(t->file_count == 1 ? " file)\n" : " files)\n");

	vga_write("total:     ");
	print_u64((uint64_t)t->total_length);
	vga_write(" bytes\n");

	vga_write("piece len: ");
	print_u64((uint64_t)t->piece_length);
	vga_write(" bytes\n");

	vga_write("pieces:    ");
	print_u64((uint64_t)t->n_pieces);
	vga_putc('\n');
}

/* --------------------------------------------------------------- self-test */

/* A minimal single-file torrent generated on the host (announce
 * "http://tracker.example.com:6969/announce", file "hello.txt" of 12 bytes,
 * piece length 16384, one 20-byte piece hash). Its infohash is known, so the
 * whole parse path is verifiable without a disk file. */
static const uint8_t sample_torrent[] = {
	0x64,0x38,0x3a,0x61,0x6e,0x6e,0x6f,0x75,0x6e,0x63,0x65,0x34,
	0x30,0x3a,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x74,0x72,0x61,
	0x63,0x6b,0x65,0x72,0x2e,0x65,0x78,0x61,0x6d,0x70,0x6c,0x65,
	0x2e,0x63,0x6f,0x6d,0x3a,0x36,0x39,0x36,0x39,0x2f,0x61,0x6e,
	0x6e,0x6f,0x75,0x6e,0x63,0x65,0x34,0x3a,0x69,0x6e,0x66,0x6f,
	0x64,0x36,0x3a,0x6c,0x65,0x6e,0x67,0x74,0x68,0x69,0x31,0x32,
	0x65,0x34,0x3a,0x6e,0x61,0x6d,0x65,0x39,0x3a,0x68,0x65,0x6c,
	0x6c,0x6f,0x2e,0x74,0x78,0x74,0x31,0x32,0x3a,0x70,0x69,0x65,
	0x63,0x65,0x20,0x6c,0x65,0x6e,0x67,0x74,0x68,0x69,0x31,0x36,
	0x33,0x38,0x34,0x65,0x36,0x3a,0x70,0x69,0x65,0x63,0x65,0x73,
	0x32,0x30,0x3a,0x22,0x59,0x63,0x63,0xb3,0xde,0x40,0xb0,0x6f,
	0x98,0x1f,0xb8,0x5d,0x82,0x31,0x2e,0x8c,0x0e,0xd5,0x11,0x65,
	0x65,
};

static const uint8_t sample_infohash[20] = {
	0x7b,0x5e,0x91,0x8f,0x36,0x49,0x08,0xaf,0xab,0x93,
	0x7e,0xcd,0xd8,0x40,0x59,0xdf,0xb6,0x11,0x02,0xb7,
};

static int name_is(const struct torrent *t, const char *s)
{
	uint32_t i = 0;
	for (; s[i]; i++)
		if (i >= t->name_len || (char)t->name[i] != s[i])
			return 0;
	return i == t->name_len;
}

int torrent_selftest(void)
{
	struct torrent t;
	int ok = 1;

	if (torrent_parse(&t, sample_torrent, (uint32_t)sizeof(sample_torrent)) != 0) {
		vga_write("torrent selftest: parse FAILED\n");
		return -1;
	}

	int hash_ok = 1;
	for (int i = 0; i < 20; i++)
		if (t.infohash[i] != sample_infohash[i]) { hash_ok = 0; break; }

	int fields_ok = name_is(&t, "hello.txt") && t.piece_length == 16384 &&
	                t.total_length == 12 && t.n_pieces == 1 &&
	                !t.multi_file && t.file_count == 1;

	torrent_print(&t);
	vga_write("  infohash  "); vga_write(hash_ok   ? "PASS\n" : "FAIL\n");
	vga_write("  fields    "); vga_write(fields_ok ? "PASS\n" : "FAIL\n");
	ok = hash_ok && fields_ok;

	torrent_free(&t);
	vga_write(ok ? "torrent selftest: PASS\n" : "torrent selftest: FAIL\n");
	return ok ? 0 : -1;
}

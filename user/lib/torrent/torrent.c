/* torrent.c — .torrent metainfo parsing + infohash (userspace libtorrent).
 * Ported from kernel/torrent.c: the node pool is a static buffer (no user-space
 * malloc) and output goes to stdout instead of the kernel console. */

#include "torrent.h"
#include "bencode.h"
#include "crypto.h"
#include "owos.h"

#define TORRENT_MAX_NODES 8192 /* ~1000 files (each a ~6-node dict) */
static struct ben_node node_pool[TORRENT_MAX_NODES];

static void op(const char *s) { puts(s); }
static void oc(char c) { write(1, &c, 1); }

static void str_to_buf(const struct ben_node *n, char *out, uint32_t cap)
{
  uint32_t i = 0, lim = (n->slen < cap - 1) ? n->slen : cap - 1;
  for (; i < lim; i++)
    out[i] = (char)n->str[i];
  out[i] = '\0';
}

static void find_announce(struct torrent *t)
{
  t->announce_buf[0] = '\0';
  t->announce = t->announce_buf;

  int a = ben_dict_get(&t->ben, t->ben.root, "announce");
  if (a >= 0 && t->ben.nodes[a].type == BEN_STR) {
    str_to_buf(&t->ben.nodes[a], t->announce_buf, (uint32_t)sizeof(t->announce_buf));
    return;
  }
  int al = ben_dict_get(&t->ben, t->ben.root, "announce-list");
  int tier = ben_list_at(&t->ben, al, 0);
  int url = ben_list_at(&t->ben, tier, 0);
  if (url >= 0 && t->ben.nodes[url].type == BEN_STR)
    str_to_buf(&t->ben.nodes[url], t->announce_buf, (uint32_t)sizeof(t->announce_buf));
}

int torrent_parse(struct torrent *t, const uint8_t *buf, uint32_t len)
{
  if (ben_parse(&t->ben, buf, len, node_pool, TORRENT_MAX_NODES) != 0 || t->ben.nodes[t->ben.root].type != BEN_DICT)
    return -1;

  int info = ben_dict_get(&t->ben, t->ben.root, "info");
  if (info < 0 || t->ben.nodes[info].type != BEN_DICT)
    goto fail;

  /* infohash = SHA-1 over the info dict's exact encoded bytes (verbatim span).
   */
  sha1(t->ben.nodes[info].raw, t->ben.nodes[info].rawlen, t->infohash);

  int pl = ben_dict_get(&t->ben, info, "piece length");
  int pc = ben_dict_get(&t->ben, info, "pieces");
  int nm = ben_dict_get(&t->ben, info, "name");
  if (pl < 0 || t->ben.nodes[pl].type != BEN_INT || pc < 0 || t->ben.nodes[pc].type != BEN_STR || nm < 0 ||
      t->ben.nodes[nm].type != BEN_STR)
    goto fail;
  if (t->ben.nodes[pc].slen % 20 != 0)
    goto fail;

  t->piece_length = t->ben.nodes[pl].ival;
  t->pieces = t->ben.nodes[pc].str;
  t->n_pieces = t->ben.nodes[pc].slen / 20;
  t->name = t->ben.nodes[nm].str;
  t->name_len = t->ben.nodes[nm].slen;

  int single = ben_dict_get(&t->ben, info, "length");
  int files = ben_dict_get(&t->ben, info, "files");
  if (single >= 0 && t->ben.nodes[single].type == BEN_INT) {
    t->multi_file = 0;
    t->file_count = 1;
    t->total_length = t->ben.nodes[single].ival;
  } else if (files >= 0 && t->ben.nodes[files].type == BEN_LIST) {
    t->multi_file = 1;
    t->file_count = ben_list_count(&t->ben, files);
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
  t->ben.nodes = (struct ben_node *)0;
  return -1;
}

void torrent_free(struct torrent *t) { t->ben.nodes = (struct ben_node *)0; /* static pool: nothing to free     */ }

/* ----------------------------------------------------------------- printing */

static void print_u64(uint64_t v)
{
  char tmp[20];
  int i = 0;
  if (v == 0) {
    oc('0');
    return;
  }
  while (v) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (i--)
    oc(tmp[i]);
}

static void print_hex20(const uint8_t h[20])
{
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    oc(hx[h[i] >> 4]);
    oc(hx[h[i] & 0xf]);
  }
}

void torrent_print(const struct torrent *t)
{
  op("name:      ");
  for (uint32_t i = 0; i < t->name_len; i++)
    oc((char)t->name[i]);
  oc('\n');
  op("infohash:  ");
  print_hex20(t->infohash);
  oc('\n');
  op("announce:  ");
  op(t->announce[0] ? t->announce : "(none)");
  oc('\n');
  op("layout:    ");
  op(t->multi_file ? "multi-file (" : "single-file (");
  print_u64((uint64_t)t->file_count);
  op(t->file_count == 1 ? " file)\n" : " files)\n");
  op("total:     ");
  print_u64((uint64_t)t->total_length);
  op(" bytes\n");
  op("piece len: ");
  print_u64((uint64_t)t->piece_length);
  op(" bytes\n");
  op("pieces:    ");
  print_u64((uint64_t)t->n_pieces);
  oc('\n');
}

/* --------------------------------------------------------------- self-test */

static const uint8_t sample_torrent[] = {
    0x64, 0x38, 0x3a, 0x61, 0x6e, 0x6e, 0x6f, 0x75, 0x6e, 0x63, 0x65, 0x34, 0x30, 0x3a, 0x68, 0x74, 0x74, 0x70, 0x3a,
    0x2f, 0x2f, 0x74, 0x72, 0x61, 0x63, 0x6b, 0x65, 0x72, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63,
    0x6f, 0x6d, 0x3a, 0x36, 0x39, 0x36, 0x39, 0x2f, 0x61, 0x6e, 0x6e, 0x6f, 0x75, 0x6e, 0x63, 0x65, 0x34, 0x3a, 0x69,
    0x6e, 0x66, 0x6f, 0x64, 0x36, 0x3a, 0x6c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x69, 0x31, 0x32, 0x65, 0x34, 0x3a, 0x6e,
    0x61, 0x6d, 0x65, 0x39, 0x3a, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e, 0x74, 0x78, 0x74, 0x31, 0x32, 0x3a, 0x70, 0x69,
    0x65, 0x63, 0x65, 0x20, 0x6c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x69, 0x31, 0x36, 0x33, 0x38, 0x34, 0x65, 0x36, 0x3a,
    0x70, 0x69, 0x65, 0x63, 0x65, 0x73, 0x32, 0x30, 0x3a, 0x22, 0x59, 0x63, 0x63, 0xb3, 0xde, 0x40, 0xb0, 0x6f, 0x98,
    0x1f, 0xb8, 0x5d, 0x82, 0x31, 0x2e, 0x8c, 0x0e, 0xd5, 0x11, 0x65, 0x65,
};
static const uint8_t sample_infohash[20] = {
    0x7b, 0x5e, 0x91, 0x8f, 0x36, 0x49, 0x08, 0xaf, 0xab, 0x93,
    0x7e, 0xcd, 0xd8, 0x40, 0x59, 0xdf, 0xb6, 0x11, 0x02, 0xb7,
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
  if (torrent_parse(&t, sample_torrent, (uint32_t)sizeof(sample_torrent)) != 0) {
    op("torrent selftest: parse FAILED\n");
    return -1;
  }
  int hash_ok = 1;
  for (int i = 0; i < 20; i++)
    if (t.infohash[i] != sample_infohash[i]) {
      hash_ok = 0;
      break;
    }
  int fields_ok = name_is(&t, "hello.txt") && t.piece_length == 16384 && t.total_length == 12 && t.n_pieces == 1 &&
                  !t.multi_file && t.file_count == 1;
  torrent_print(&t);
  op("  infohash  ");
  op(hash_ok ? "PASS\n" : "FAIL\n");
  op("  fields    ");
  op(fields_ok ? "PASS\n" : "FAIL\n");
  int ok = hash_ok && fields_ok;
  torrent_free(&t);
  op(ok ? "torrent selftest: PASS\n" : "torrent selftest: FAIL\n");
  return ok ? 0 : -1;
}

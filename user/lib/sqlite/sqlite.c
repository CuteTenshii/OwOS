/* sqlite.c — the SQLite3 file-format engine described in sqlite.h.
 *
 * Writer: build each table as a table b-tree bottom-up. Rows become records
 * (record header of serial-type varints + the column bodies); records become
 * table-leaf cells; cells pack into leaf pages; if a table spills more than one
 * leaf, interior pages are stacked on top until a single root remains. A record
 * too large for one leaf keeps OVF_X bytes local and spills the rest onto a
 * chain of overflow pages. The schema table (sqlite_schema) is one leaf rooted
 * on page 1, after the 100-byte database header.
 *
 * Reader: parse the header, walk the schema b-tree to discover tables and their
 * root pages, then walk each table's b-tree (leaf and interior) decoding every
 * record back into sql_value columns, reassembling overflow chains as needed.
 *
 * All multi-byte on-disk integers are big-endian. Varints are SQLite's 1-9 byte
 * big-endian form (high bit = continuation; the 9th byte uses all 8 bits).
 */
#include "owos.h"
#include "sqlite.h"

#define PAGESIZE       4096          /* power of two in [512, 65536]            */
#define HDR_BYTES      100           /* database header length (page 1 prefix)  */
#define SQLITE_VERNUM  3045000       /* recorded in the header (cosmetic)       */

#define BT_LEAF        0x0D          /* table b-tree leaf                       */
#define BT_INTERIOR    0x05          /* table b-tree interior                   */

/* Overflow thresholds for a table b-tree leaf, per the SQLite file format. We
 * always write (and only read) pages with zero reserved bytes, so the usable
 * page size U equals PAGESIZE. X is the most payload a leaf cell may keep local
 * before spilling; M is the minimum kept local once it does. The 64/32/32
 * payload fractions the header records are baked into these constants. */
#define OVF_U          PAGESIZE
#define OVF_X          (OVF_U - 35)
#define OVF_M          (((OVF_U - 12) * 32 / 255) - 23)

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* ------------------------------------------------------------ byte helpers */

static void put16(u8 *p, unsigned v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v)
{ p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static unsigned get16(const u8 *p) { return ((unsigned)p[0] << 8) | p[1]; }
static u32 get32(const u8 *p)
{ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* SQLite varint. put_varint writes v into p (<=9 bytes) and returns the count. */
static int put_varint(u8 *p, u64 v)
{
	if (v & (((u64)0xff000000u) << 32)) {     /* needs all 64 bits -> 9 bytes */
		p[8] = (u8)v;
		v >>= 8;
		for (int i = 7; i >= 0; i--) { p[i] = (u8)((v & 0x7f) | 0x80); v >>= 7; }
		return 9;
	}
	u8 buf[9];
	int n = 0;
	do { buf[n++] = (u8)(v & 0x7f); v >>= 7; } while (v);
	for (int i = 0, j = n - 1; j >= 0; j--, i++)
		p[i] = (u8)(buf[j] | (j ? 0x80 : 0x00));
	return n;
}

static int varint_len(u64 v) { u8 t[9]; return put_varint(t, v); }

/* How many bytes of a `plen`-byte table-leaf payload stay on the leaf page. When
 * the return value is < plen the remainder spills onto overflow pages, with a
 * 4-byte overflow-page pointer following the local bytes. */
static int leaf_local(int plen)
{
	if (plen <= OVF_X) return plen;
	int k = OVF_M + (plen - OVF_M) % (OVF_U - 4);
	return (k <= OVF_X) ? k : OVF_M;
}

/* Decode a varint from p (at most maxlen bytes). Returns bytes consumed, or 0
 * if it runs off the end. */
static int get_varint(const u8 *p, int maxlen, u64 *out)
{
	u64 v = 0;
	for (int i = 0; i < 8; i++) {
		if (i >= maxlen) return 0;
		v = (v << 7) | (p[i] & 0x7f);
		if (!(p[i] & 0x80)) { *out = v; return i + 1; }
	}
	if (8 >= maxlen) return 0;
	v = (v << 8) | p[8];
	*out = v;
	return 9;
}

/* ---------------------------------------------------- serial-type encoding */

/* The SQLite serial type for an integer value, choosing the smallest width. */
static int int_serial(long long v)
{
	if (v == 0) return 8;
	if (v == 1) return 9;
	if (v >= -128 && v <= 127) return 1;
	if (v >= -32768 && v <= 32767) return 2;
	if (v >= -8388608 && v <= 8388607) return 3;
	if (v >= -2147483648LL && v <= 2147483647LL) return 4;
	if (v >= -140737488355328LL && v <= 140737488355327LL) return 5;
	return 6;
}

/* On-disk content length of an integer serial type. */
static int int_content_len(int st)
{
	switch (st) {
	case 1: return 1; case 2: return 2; case 3: return 3;
	case 4: return 4; case 5: return 6; case 6: return 8;
	default: return 0;            /* 8 and 9 carry the value in the type      */
	}
}

/* Store v as an n-byte big-endian two's-complement integer. */
static void put_int_be(u8 *p, long long v, int n)
{
	u64 uv = (u64)v;
	for (int i = 0; i < n; i++) p[n - 1 - i] = (u8)(uv >> (8 * i));
}

/* Read an n-byte big-endian two's-complement integer, sign-extended. */
static long long get_int_be(const u8 *p, int n)
{
	long long v = 0;
	for (int i = 0; i < n; i++) v = (v << 8) | p[i];
	if (n < 8 && (p[0] & 0x80)) v |= (~0LL) << (8 * n);
	return v;
}

/* ------------------------------------------------------------ record build */

/* Encode one row's columns into an SQLite record (header + bodies). Returns a
 * malloc'd buffer and writes its length to outlen, or 0 on OOM. */
static u8 *build_record(const struct sql_value *vals, int ncol, int *outlen)
{
	int st[SQL_MAX_COLS], cl[SQL_MAX_COLS];
	int stbytes = 0, content = 0;

	for (int c = 0; c < ncol; c++) {
		const struct sql_value *v = &vals[c];
		if (v->type == SQL_INT) {
			st[c] = int_serial(v->i);
			cl[c] = int_content_len(st[c]);
		} else if (v->type == SQL_TEXT) {
			st[c] = 13 + 2 * v->len;
			cl[c] = v->len;
		} else if (v->type == SQL_BLOB) {
			st[c] = 12 + 2 * v->len;
			cl[c] = v->len;
		} else {                                /* SQL_NULL */
			st[c] = 0;
			cl[c] = 0;
		}
		stbytes += varint_len((u64)st[c]);
		content += cl[c];
	}

	/* Header size includes the varint that encodes it; iterate to a fixpoint. */
	int hdrlen = 1, header_size;
	for (;;) {
		header_size = hdrlen + stbytes;
		if (varint_len((u64)header_size) == hdrlen) break;
		hdrlen = varint_len((u64)header_size);
	}

	int total = header_size + content;
	u8 *buf = (u8 *)malloc((unsigned)total);
	if (!buf) return 0;

	u8 *p = buf;
	p += put_varint(p, (u64)header_size);
	for (int c = 0; c < ncol; c++) p += put_varint(p, (u64)st[c]);
	for (int c = 0; c < ncol; c++) {
		const struct sql_value *v = &vals[c];
		if (v->type == SQL_INT) { put_int_be(p, v->i, cl[c]); p += cl[c]; }
		else if (v->type == SQL_TEXT || v->type == SQL_BLOB) {
			if (cl[c]) memcpy(p, v->text, (__SIZE_TYPE__)cl[c]);
			p += cl[c];
		}
	}
	*outlen = total;
	return buf;
}

/* ------------------------------------------------------------- page output */

struct cell { u8 *buf; int len; long long key; };

struct outpage { int page_no; u8 *buf; };

struct builder {
	struct outpage *pages;
	int             cap, n;
	int             next_page;       /* next data page number to hand out      */
	int             err;
};

/* Reserve an output page with a fixed page number; returns its zeroed buffer. */
static u8 *add_page(struct builder *b, int page_no)
{
	if (b->n == b->cap) {
		int nc = b->cap ? b->cap * 2 : 16;
		struct outpage *np = (struct outpage *)realloc(b->pages,
		                                  (unsigned)(nc * (int)sizeof *np));
		if (!np) { b->err = 1; return 0; }
		b->pages = np; b->cap = nc;
	}
	u8 *buf = (u8 *)calloc(1, PAGESIZE);
	if (!buf) { b->err = 1; return 0; }
	b->pages[b->n].page_no = page_no;
	b->pages[b->n].buf = buf;
	b->n++;
	return buf;
}

/* Render a table-leaf page. hdr_off is 100 for page 1 (after the db header),
 * else 0. Cells are placed from the end of the page downward; their pointers
 * (absolute page offsets) follow the 8-byte page header. */
static void render_leaf(u8 *page, int hdr_off, struct cell *cells,
                        int first, int count)
{
	u8 *h = page + hdr_off;
	h[0] = BT_LEAF;
	put16(h + 1, 0);                 /* first freeblock                        */
	put16(h + 3, (unsigned)count);   /* cell count                             */
	h[7] = 0;                        /* fragmented free bytes                  */

	u8 *ptr = h + 8;
	int content = PAGESIZE;
	for (int i = 0; i < count; i++) {
		const struct cell *c = &cells[first + i];
		/* Defensive: a cell that would start before the cell-pointer array (or
		 * before the page) must never be written — that would corrupt the heap
		 * below the page buffer. The caller (build_btree) already rejects an
		 * oversized record, so this only guards against a packing miscount. */
		if (content - c->len < (int)(ptr + 2 - page)) break;
		content -= c->len;
		memcpy(page + content, c->buf, (__SIZE_TYPE__)c->len);
		put16(ptr, (unsigned)content);
		ptr += 2;
	}
	put16(h + 5, content == 65536 ? 0 : (unsigned)content);  /* content start  */
}

/* An interior child reference: its page number and the largest rowid below it. */
struct child { int page; long long maxkey; };

/* Render a table-interior page over children[first..first+count-1]. The last
 * child becomes the page's right-most pointer; the rest become cells. */
static void render_interior(u8 *page, const struct child *children,
                            int first, int count)
{
	int ncell = count - 1;
	u8 *h = page;
	h[0] = BT_INTERIOR;
	put16(h + 1, 0);
	put16(h + 3, (unsigned)ncell);
	h[7] = 0;
	put32(h + 8, (u32)children[first + count - 1].page);  /* right-most child  */

	u8 *ptr = h + 12;
	int content = PAGESIZE;
	for (int i = 0; i < ncell; i++) {
		const struct child *c = &children[first + i];
		u8 tmp[4 + 9];
		put32(tmp, (u32)c->page);
		int kl = put_varint(tmp + 4, (u64)c->maxkey);
		int len = 4 + kl;
		content -= len;
		memcpy(page + content, tmp, (__SIZE_TYPE__)len);
		put16(ptr, (unsigned)content);
		ptr += 2;
	}
	put16(h + 5, content == 65536 ? 0 : (unsigned)content);
}

/* Build a table b-tree from `cells` (already in key order) and return its root
 * page number. Always produces at least one (possibly empty) leaf. */
static int build_btree(struct builder *b, struct cell *cells, int ncells)
{
	/* Level 0: pack cells into leaf pages. */
	int max_leaves = ncells + 1;
	struct child *level = (struct child *)malloc(
	                          (unsigned)(max_leaves * (int)sizeof(struct child)));
	if (!level) { b->err = 1; return 0; }
	int nlevel = 0;

	if (ncells == 0) {
		int pn = b->next_page++;
		u8 *pg = add_page(b, pn);
		if (!pg) { free(level); return 0; }
		render_leaf(pg, 0, cells, 0, 0);
		level[nlevel].page = pn; level[nlevel].maxkey = 0; nlevel++;
	} else {
		int i = 0;
		while (i < ncells) {
			int first = i, used = 8;
			long long maxkey = cells[i].key;
			while (i < ncells) {
				int need = 2 + cells[i].len;
				/* Every cell is built by build_leaf_cell, which caps the local
				 * payload at OVF_X and spills the rest to overflow pages, so a
				 * cell can never exceed a page. This stays only as defense: were
				 * an over-large cell to slip through, render_leaf would memcpy
				 * past the page buffer (content = PAGESIZE - len going negative). */
				if (need > PAGESIZE - 8) { free(level); b->err = 1; return 0; }
				if (used + need > PAGESIZE && i > first) break;
				used += need;
				maxkey = cells[i].key;
				i++;
			}
			int pn = b->next_page++;
			u8 *pg = add_page(b, pn);
			if (!pg) { free(level); return 0; }
			render_leaf(pg, 0, cells, first, i - first);
			level[nlevel].page = pn; level[nlevel].maxkey = maxkey; nlevel++;
		}
	}

	/* Reduce upward with interior levels until a single root remains. */
	while (nlevel > 1) {
		struct child *up = (struct child *)malloc(
		                       (unsigned)((nlevel) * (int)sizeof(struct child)));
		if (!up) { free(level); b->err = 1; return 0; }
		int nup = 0, i = 0;
		while (i < nlevel) {
			int first = i, used = 12;
			int j = i;                       /* children [first..j] this page  */
			for (;;) {
				if (j + 1 > nlevel - 1) break;
				int addcost = 2 + 4 + varint_len((u64)level[j].maxkey);
				if (used + addcost > PAGESIZE) break;
				used += addcost;
				j++;
			}
			int pn = b->next_page++;
			u8 *pg = add_page(b, pn);
			if (!pg) { free(level); free(up); return 0; }
			render_interior(pg, level, first, j - first + 1);
			up[nup].page = pn;
			up[nup].maxkey = level[j].maxkey;   /* subtree max = last child's  */
			nup++;
			i = j + 1;
		}
		free(level);
		level = up;
		nlevel = nup;
	}

	int root = level[0].page;
	free(level);
	return root;
}

/* --------------------------------------------------------- in-memory model */

struct sql_db *sql_new(void)
{
	struct sql_db *db = (struct sql_db *)calloc(1, sizeof *db);
	return db;
}

struct sql_table *sql_table_find(struct sql_db *db, const char *name)
{
	for (struct sql_table *t = db->tables; t; t = t->next)
		if (streq(t->name, name)) return t;
	return 0;
}

static struct sql_table *table_add(struct sql_db *db, const char *name,
                                   const char *create_sql, int ncol)
{
	struct sql_table *t = (struct sql_table *)calloc(1, sizeof *t);
	if (!t) return 0;
	int k = 0;
	for (; name[k] && k < (int)sizeof t->name - 1; k++) t->name[k] = name[k];
	t->name[k] = '\0';
	if (create_sql) {
		int n = strlen(create_sql);
		t->sql = (char *)malloc((unsigned)(n + 1));
		if (t->sql) { memcpy(t->sql, create_sql, (__SIZE_TYPE__)n); t->sql[n] = '\0'; }
	}
	t->ncol = ncol;
	t->next_rowid = 1;
	t->next = db->tables;
	db->tables = t;
	return t;
}

struct sql_table *sql_create_table(struct sql_db *db, const char *name,
                                   const char *create_sql, int ncol)
{
	if (sql_table_find(db, name)) return 0;
	return table_add(db, name, create_sql, ncol);
}

static void free_value(struct sql_value *v)
{
	if ((v->type == SQL_TEXT || v->type == SQL_BLOB) && v->text) free(v->text);
	v->text = 0;
}

static void free_row(struct sql_row *r)
{
	for (int c = 0; c < r->ncol; c++) free_value(&r->col[c]);
	free(r);
}

void sql_truncate(struct sql_table *t)
{
	struct sql_row *r = t->rows;
	while (r) { struct sql_row *n = r->next; free_row(r); r = n; }
	t->rows = t->tail = 0;
	t->nrows = 0;
	t->next_rowid = 1;
}

/* Append a row carrying values already owned by it (text buffers transferred). */
static int row_append(struct sql_table *t, long long rowid,
                      struct sql_value *vals, int ncol)
{
	struct sql_row *r = (struct sql_row *)calloc(1, sizeof *r);
	if (!r) return -1;
	if (ncol > SQL_MAX_COLS) ncol = SQL_MAX_COLS;
	r->rowid = rowid;
	r->ncol = ncol;
	for (int c = 0; c < ncol; c++) r->col[c] = vals[c];
	r->next = 0;
	if (t->tail) t->tail->next = r; else t->rows = r;
	t->tail = r;
	t->nrows++;
	if (rowid >= t->next_rowid) t->next_rowid = rowid + 1;
	return 0;
}

int sql_insert(struct sql_table *t, const struct sql_value *vals, int ncol)
{
	if (ncol > SQL_MAX_COLS) ncol = SQL_MAX_COLS;
	struct sql_value copy[SQL_MAX_COLS];
	int made = 0;
	for (int c = 0; c < ncol; c++) {
		copy[c] = vals[c];
		copy[c].text = 0;
		if (vals[c].type == SQL_TEXT || vals[c].type == SQL_BLOB) {
			int len = vals[c].len;
			char *d = (char *)malloc((unsigned)(len + 1));
			if (!d) { for (int k = 0; k < made; k++) free_value(&copy[k]); return -1; }
			if (len) memcpy(d, vals[c].text, (__SIZE_TYPE__)len);
			d[len] = '\0';
			copy[c].text = d;
			copy[c].len = len;
			made++;
		}
	}
	return row_append(t, t->next_rowid, copy, ncol);
}

void sql_close(struct sql_db *db)
{
	if (!db) return;
	struct sql_table *t = db->tables;
	while (t) {
		struct sql_table *nt = t->next;
		sql_truncate(t);
		if (t->sql) free(t->sql);
		free(t);
		t = nt;
	}
	free(db);
}

struct sql_value sqlv_text(const char *s)
{
	struct sql_value x;
	x.type = SQL_TEXT;
	x.i = 0;
	x.text = (char *)s;
	x.len = s ? strlen(s) : 0;
	return x;
}

/* ----------------------------------------------------------------- writer */

/* Build a table-leaf cell from an already-encoded record payload, spilling the
 * remainder onto a chain of overflow pages when the payload exceeds the leaf
 * capacity. The cell is varint(payload len) + varint(rowid) + local bytes, plus
 * a 4-byte first-overflow-page pointer when it spills. Overflow pages are
 * allocated from the builder here (before the b-tree pages), each carrying a
 * 4-byte next-page pointer (0 at the tail) then up to OVF_U-4 payload bytes.
 * Returns a malloc'd cell buffer and its length via *outlen, or 0 (b->err set). */
static u8 *build_leaf_cell(struct builder *b, const u8 *payload, int plen,
                           long long rowid, int *outlen)
{
	int local = leaf_local(plen);
	int spills = local < plen;
	int celllen = varint_len((u64)plen) + varint_len((u64)rowid) + local
	              + (spills ? 4 : 0);
	u8 *cb = (u8 *)malloc((unsigned)celllen);
	if (!cb) { b->err = 1; return 0; }

	int o = put_varint(cb, (u64)plen);
	o += put_varint(cb + o, (u64)rowid);
	memcpy(cb + o, payload, (__SIZE_TYPE__)local);
	o += local;

	if (spills) {
		int first = b->next_page++;
		put32(cb + o, (u32)first);

		int rem = plen - local;
		const u8 *src = payload + local;
		int page_no = first;
		while (rem > 0) {
			u8 *pg = add_page(b, page_no);
			if (!pg) { free(cb); return 0; }   /* b->err set by add_page */
			int chunk = rem > OVF_U - 4 ? OVF_U - 4 : rem;
			int next = rem > chunk ? b->next_page++ : 0;
			put32(pg, (u32)next);
			memcpy(pg + 4, src, (__SIZE_TYPE__)chunk);
			src += chunk; rem -= chunk;
			page_no = next;
		}
	}

	*outlen = celllen;
	return cb;
}

int sql_save(struct sql_db *db, const char *path)
{
	struct builder b;
	b.pages = 0; b.cap = 0; b.n = 0; b.next_page = 2; b.err = 0;

	int ntab = 0;
	for (struct sql_table *t = db->tables; t; t = t->next) ntab++;

	int *roots = 0;
	if (ntab) {
		roots = (int *)malloc((unsigned)(ntab * (int)sizeof(int)));
		if (!roots) return -1;
	}

	/* Build each table's b-tree, recording its root page. */
	int ti = 0;
	for (struct sql_table *t = db->tables; t; t = t->next, ti++) {
		struct cell *cells = 0;
		if (t->nrows) {
			cells = (struct cell *)malloc(
			            (unsigned)(t->nrows * (int)sizeof(struct cell)));
			if (!cells) { b.err = 1; break; }
		}
		int nc = 0;
		long long rid = 1;
		for (struct sql_row *r = t->rows; r; r = r->next) {
			int reclen;
			u8 *rec = build_record(r->col, r->ncol, &reclen);
			if (!rec) { b.err = 1; break; }
			int celllen;
			u8 *cb = build_leaf_cell(&b, rec, reclen, r->rowid, &celllen);
			free(rec);
			if (!cb) { b.err = 1; break; }
			cells[nc].buf = cb;
			cells[nc].len = celllen;
			cells[nc].key = r->rowid;
			nc++;
			rid = r->rowid;
		}
		(void)rid;
		int root = b.err ? 0 : build_btree(&b, cells, nc);
		for (int k = 0; k < nc; k++) free(cells[k].buf);
		if (cells) free(cells);
		roots[ti] = root;
		if (b.err) break;
	}

	/* Build the schema (sqlite_schema) rows and lay them on page 1. */
	if (!b.err) {
		struct cell *scells = 0;
		if (ntab) {
			scells = (struct cell *)malloc(
			             (unsigned)(ntab * (int)sizeof(struct cell)));
			if (!scells) b.err = 1;
		}
		int sn = 0;
		ti = 0;
		for (struct sql_table *t = db->tables; t && !b.err; t = t->next, ti++) {
			struct sql_value sv[5];
			sv[0] = sqlv_text("table");
			sv[1] = sqlv_text(t->name);
			sv[2] = sqlv_text(t->name);
			sv[3] = sqlv_int(roots[ti]);
			sv[4] = sqlv_text(t->sql ? t->sql : "");
			int reclen;
			u8 *rec = build_record(sv, 5, &reclen);
			if (!rec) { b.err = 1; break; }
			int celllen;                               /* schema rowid 1,2,... */
			u8 *cb = build_leaf_cell(&b, rec, reclen, sn + 1, &celllen);
			free(rec);
			if (!cb) { b.err = 1; break; }
			scells[sn].buf = cb;
			scells[sn].len = celllen;
			scells[sn].key = sn + 1;
			sn++;
		}
		if (!b.err) {
			u8 *page1 = add_page(&b, 1);
			if (page1) render_leaf(page1, HDR_BYTES, scells, 0, sn);
		}
		for (int k = 0; k < sn; k++) free(scells[k].buf);
		if (scells) free(scells);
	}

	int rc = -1;
	if (!b.err) {
		int total_pages = b.next_page - 1;       /* pages 1..(next_page-1)     */
		if (total_pages < 1) total_pages = 1;
		unsigned filelen = (unsigned)total_pages * PAGESIZE;
		u8 *file = (u8 *)calloc(1, filelen);
		if (file) {
			for (int i = 0; i < b.n; i++)
				memcpy(file + (b.pages[i].page_no - 1) * PAGESIZE,
				       b.pages[i].buf, PAGESIZE);

			/* The 100-byte database header overlays the start of page 1. */
			static const char magic[16] = "SQLite format 3";
			memcpy(file, magic, 16);
			put16(file + 16, PAGESIZE == 65536 ? 1 : PAGESIZE);
			file[18] = 1;                /* file format write version (legacy) */
			file[19] = 1;                /* file format read version           */
			file[20] = 0;                /* reserved bytes per page            */
			file[21] = 64;               /* max embedded payload fraction      */
			file[22] = 32;               /* min embedded payload fraction      */
			file[23] = 32;               /* leaf payload fraction              */
			put32(file + 24, 1);         /* file change counter                */
			put32(file + 28, (u32)total_pages);   /* size in pages            */
			put32(file + 32, 0);         /* first freelist trunk page          */
			put32(file + 36, 0);         /* freelist page count                */
			put32(file + 40, 1);         /* schema cookie                      */
			put32(file + 44, 4);         /* schema format number               */
			put32(file + 48, 0);         /* default page cache size            */
			put32(file + 52, 0);         /* largest root page (vacuum)         */
			put32(file + 56, 1);         /* text encoding = UTF-8              */
			put32(file + 60, 0);         /* user version                       */
			put32(file + 64, 0);         /* incremental vacuum mode            */
			put32(file + 68, 0);         /* application id                     */
			/* file+72..91 reserved (already zero from calloc)                 */
			put32(file + 92, 1);         /* version-valid-for (== change ctr)  */
			put32(file + 96, SQLITE_VERNUM);

			rc = writefile(path, file, (int)filelen);
			free(file);
		}
	}

	for (int i = 0; i < b.n; i++) free(b.pages[i].buf);
	if (b.pages) free(b.pages);
	if (roots) free(roots);
	return rc;
}

/* ----------------------------------------------------------------- reader */

/* Decode an SQLite record into owned sql_values. Returns the column count. */
static int decode_record(const u8 *payload, int plen,
                         struct sql_value *out, int maxcol)
{
	u64 hsize;
	int k = get_varint(payload, plen, &hsize);
	if (!k || (int)hsize > plen) return 0;

	const u8 *hp = payload + k;
	int hrem = (int)hsize - k;
	const u8 *bp = payload + (int)hsize;
	int brem = plen - (int)hsize;
	int nc = 0;

	while (hrem > 0 && nc < maxcol) {
		u64 st;
		int kk = get_varint(hp, hrem, &st);
		if (!kk) break;
		hp += kk; hrem -= kk;

		struct sql_value v;
		v.type = SQL_NULL; v.i = 0; v.text = 0; v.len = 0;

		if (st == 0) {
			/* NULL */
		} else if (st >= 1 && st <= 6) {
			int n = int_content_len((int)st);
			if (n > brem) break;
			v.type = SQL_INT; v.i = get_int_be(bp, n);
			bp += n; brem -= n;
		} else if (st == 7) {
			if (8 > brem) break;
			bp += 8; brem -= 8;          /* float: stored as NULL in the model */
		} else if (st == 8) {
			v.type = SQL_INT; v.i = 0;
		} else if (st == 9) {
			v.type = SQL_INT; v.i = 1;
		} else if (st >= 12) {
			u64 slen = (st - 12) / 2;
			int isText = (st & 1);
			if (slen > (u64)brem) break;   /* also rejects huge st that would go negative as int */
			int len = (int)slen;
			char *d = (char *)malloc((unsigned)(len + 1));
			if (!d) break;
			if (len) memcpy(d, bp, (__SIZE_TYPE__)len);
			d[len] = '\0';
			v.type = isText ? SQL_TEXT : SQL_BLOB;
			v.text = d; v.len = len;
			bp += len; brem -= len;
		} else {
			/* reserved (10, 11) — treat as NULL */
		}
		out[nc++] = v;
	}
	return nc;
}

/* Per-cell callback while walking a table b-tree. */
typedef void (*row_fn)(void *ctx, long long rowid, const u8 *payload, int plen);

/* Reassemble an overflowing table-leaf payload into one contiguous buffer.
 * `local_ptr` points at the local payload on the leaf; `local` bytes are there,
 * immediately followed by a 4-byte first-overflow-page pointer. The chain is
 * walked, each page contributing up to OVF_U-4 bytes after its own 4-byte
 * next-page pointer, until `plen` bytes are gathered. `visited` guards against a
 * cyclic or file-overrunning chain. Returns a malloc'd buffer of plen bytes
 * (caller frees) or 0 on OOM/corruption. */
static u8 *read_overflow(const u8 *file, unsigned filelen, const u8 *local_ptr,
                         int plen, int local, u8 *visited)
{
	u8 *buf = (u8 *)malloc((unsigned)plen);
	if (!buf) return 0;
	memcpy(buf, local_ptr, (__SIZE_TYPE__)local);
	int got = local;
	int next = (int)get32(local_ptr + local);

	while (got < plen) {
		if (next < 1) { free(buf); return 0; }
		u64 off = (u64)(next - 1) * PAGESIZE;
		if (off + PAGESIZE > filelen) { free(buf); return 0; }
		if (visited[next >> 3] & (1 << (next & 7))) { free(buf); return 0; }
		visited[next >> 3] |= (u8)(1 << (next & 7));

		const u8 *op = file + off;
		int chunk = plen - got;
		if (chunk > OVF_U - 4) chunk = OVF_U - 4;
		memcpy(buf + got, op + 4, (__SIZE_TYPE__)chunk);
		got += chunk;
		next = (int)get32(op);
	}
	return buf;
}

static void walk_r(const u8 *file, unsigned filelen, int page_no,
                   int depth, row_fn fn, void *ctx, u8 *visited)
{
	if (depth > 32 || page_no < 1) return;
	/* 64-bit so a hostile page number can't wrap the bounds check. */
	u64 off = (u64)(page_no - 1) * PAGESIZE;
	if (off + PAGESIZE > filelen) return;
	/* page_no is now known <= filelen/PAGESIZE, so the bitmap index is in range;
	 * refuse to re-descend a page to kill cyclic/DAG b-trees (exponential walk). */
	if (visited[page_no >> 3] & (1 << (page_no & 7))) return;
	visited[page_no >> 3] |= (u8)(1 << (page_no & 7));

	const u8 *pg = file + off;
	int hdr_off = (page_no == 1) ? HDR_BYTES : 0;
	const u8 *h = pg + hdr_off;
	int type = h[0];
	int ncell = (int)get16(h + 3);

	if (type == BT_LEAF) {
		const u8 *cptr = h + 8;
		int maxcell = (PAGESIZE - hdr_off - 8) / 2;   /* cell-ptr array must fit the page */
		if (ncell > maxcell) ncell = maxcell;
		for (int i = 0; i < ncell; i++) {
			int cp = (int)get16(cptr + i * 2);
			if (cp < hdr_off || cp >= PAGESIZE) continue;
			const u8 *c = pg + cp;
			int rem = PAGESIZE - cp;
			u64 plen, rowid;
			int k = get_varint(c, rem, &plen);
			if (!k) continue;
			c += k; rem -= k;
			int k2 = get_varint(c, rem, &rowid);
			if (!k2) continue;
			c += k2; rem -= k2;
			if (plen > (u64)filelen) continue;   /* payload can't exceed file  */
			int P = (int)plen;
			int local = leaf_local(P);
			if (local == P) {
				if (P > rem) continue;           /* corrupt: claims to fit      */
				fn(ctx, (long long)rowid, c, P);
			} else {
				if (local + 4 > rem) continue;   /* local + ovf ptr must be here */
				u8 *asm_buf = read_overflow(file, filelen, c, P, local, visited);
				if (!asm_buf) continue;
				fn(ctx, (long long)rowid, asm_buf, P);
				free(asm_buf);
			}
		}
	} else if (type == BT_INTERIOR) {
		const u8 *cptr = h + 12;
		int maxcell = (PAGESIZE - hdr_off - 12) / 2;
		if (ncell > maxcell) ncell = maxcell;
		for (int i = 0; i < ncell; i++) {
			int cp = (int)get16(cptr + i * 2);
			if (cp < hdr_off || cp + 4 > PAGESIZE) continue;
			int child = (int)get32(pg + cp);
			walk_r(file, filelen, child, depth + 1, fn, ctx, visited);
		}
		int right = (int)get32(h + 8);
		walk_r(file, filelen, right, depth + 1, fn, ctx, visited);
	}
}

static void walk_table(const u8 *file, unsigned filelen, int page_no,
                       row_fn fn, void *ctx)
{
	unsigned npages = filelen / PAGESIZE;
	u8 *visited = (u8 *)calloc(1, npages / 8 + 1);
	if (!visited) return;
	walk_r(file, filelen, page_no, 0, fn, ctx, visited);
	free(visited);
}

/* Count top-level columns in a "CREATE TABLE x(...)" string (best effort). */
static int count_cols(const char *sql)
{
	if (!sql) return 0;
	const char *p = sql;
	while (*p && *p != '(') p++;
	if (!*p) return 0;
	p++;
	int depth = 1, cols = 1;
	for (; *p && depth > 0; p++) {
		if (*p == '(') depth++;
		else if (*p == ')') depth--;
		else if (*p == ',' && depth == 1) cols++;
	}
	return cols;
}

struct schema_ctx { struct sql_db *db; };

static void schema_cb(void *ctx, long long rowid, const u8 *payload, int plen)
{
	(void)rowid;
	struct schema_ctx *sc = (struct schema_ctx *)ctx;
	struct sql_value v[8];
	int nc = decode_record(payload, plen, v, 8);
	if (nc >= 5 && v[0].type == SQL_TEXT && streq(v[0].text, "table") &&
	    v[1].type == SQL_TEXT && v[3].type == SQL_INT) {
		const char *sql = (v[4].type == SQL_TEXT) ? v[4].text : 0;
		struct sql_table *t = table_add(sc->db, v[1].text, sql, count_cols(sql));
		if (t) t->load_root = (int)v[3].i;
	}
	for (int c = 0; c < nc; c++) free_value(&v[c]);
}

struct load_ctx { struct sql_table *table; };

static void row_cb(void *ctx, long long rowid, const u8 *payload, int plen)
{
	struct load_ctx *lc = (struct load_ctx *)ctx;
	struct sql_value v[SQL_MAX_COLS];
	int nc = decode_record(payload, plen, v, SQL_MAX_COLS);
	if (row_append(lc->table, rowid, v, nc) != 0)
		for (int c = 0; c < nc; c++) free_value(&v[c]);
}

/* Read an entire file into a freshly malloc'd buffer, growing as needed (no
 * fixed size cap — the buffer is exactly as big as the file). *outlen gets the
 * byte count; returns the buffer (caller frees), or 0 on OOM. A missing file
 * reads as zero bytes. */
static u8 *read_whole(const char *path, int *outlen)
{
	unsigned cap = 8192, len = 0;
	u8 *file = (u8 *)malloc(cap);
	if (!file) return 0;
	for (;;) {
		if (len == cap) {
			unsigned nc = cap * 2;
			u8 *nf = (u8 *)realloc(file, nc);
			if (!nf) { free(file); return 0; }
			file = nf; cap = nc;
		}
		int got = readfile_at(path, len, file + len, cap - len);
		if (got <= 0) break;          /* 0 = EOF, -1 = absent/error            */
		len += (unsigned)got;
	}
	*outlen = (int)len;
	return file;
}

struct sql_db *sql_open(const char *path)
{
	int n = 0;
	u8 *file = read_whole(path, &n);
	if (!file) return 0;

	struct sql_db *db = sql_new();
	if (!db) { free(file); return 0; }

	if (n < HDR_BYTES) { free(file); return db; }   /* absent/empty -> blank  */

	static const char magic[16] = "SQLite format 3";
	for (int i = 0; i < 15; i++)
		if (file[i] != (u8)magic[i]) { free(file); sql_close(db); return 0; }

	unsigned ps = get16(file + 16);
	unsigned want = (ps == 1) ? 65536 : ps;
	if (want != PAGESIZE) { free(file); sql_close(db); return 0; }
	if ((unsigned)n < PAGESIZE) { free(file); sql_close(db); return 0; }
	/* Reserved bytes per page shrink the usable size U; our overflow thresholds
	 * assume U == PAGESIZE, so a file that reserves any can't be read correctly. */
	if (file[20] != 0) { free(file); sql_close(db); return 0; }

	struct schema_ctx sc; sc.db = db;
	walk_table(file, (unsigned)n, 1, schema_cb, &sc);

	for (struct sql_table *t = db->tables; t; t = t->next) {
		if (t->load_root > 0) {
			struct load_ctx lc; lc.table = t;
			walk_table(file, (unsigned)n, t->load_root, row_cb, &lc);
		}
	}

	free(file);
	return db;
}

/* rar.c - RAR archive handle: open, iterate entries across volumes, extract with integrity checks. */
#include "crc32.h"
#include "owos.h"
#include "rar_int.h"

const char *rar_strerror(int err)
{
  switch (err) {
  case RAR_OK:
    return "ok";
  case RAR_EIO:
    return "read error";
  case RAR_EFORMAT:
    return "not a RAR archive or damaged header";
  case RAR_ECRC:
    return "checksum mismatch";
  case RAR_EPASSWORD:
    return "password required or incorrect";
  case RAR_EUNSUPPORTED:
    return "unsupported feature";
  case RAR_ENOMEM:
    return "out of memory or over the memory limit";
  case RAR_ELIMIT:
    return "extraction size limit reached";
  case RAR_EWRITE:
    return "write failed";
  case RAR_EVOLUME:
    return "missing volume";
  case RAR_ECORRUPT:
    return "corrupt data";
  case RAR_EOLD:
    return "unsupported (old RAR)";
  case RAR_ESTATE:
    return "entry depends on data that was skipped";
  }
  return "unknown error";
}

void *rar_alloc(struct rar *r, size_t n)
{
  if (n > r->max_mem - r->mem)
    return NULL;
  void *p = calloc(1, n ? n : 1);
  if (p)
    r->mem += n;
  return p;
}

void *rar_realloc(struct rar *r, void *p, size_t old, size_t n)
{
  if (n > old && n - old > r->max_mem - r->mem)
    return NULL;
  void *q = realloc(p, n ? n : 1);
  if (!q)
    return NULL;
  if (n > old)
    memset((uint8_t *)q + old, 0, n - old);
  r->mem = r->mem - old + n;
  return q;
}

void rar_unalloc(struct rar *r, void *p, size_t n)
{
  if (p) {
    free(p);
    r->mem -= n;
  }
}

int rar_vol_read(struct rar *r, struct rar_vol *v, uint64_t off, void *buf, uint32_t len)
{
  (void)r;
  uint32_t got = 0;
  while (got < len) {
    int n = v->src.read(v->src.ctx, off + got, (uint8_t *)buf + got, len - got);
    if (n < 0)
      return RAR_EIO;
    if (n == 0)
      break;
    got += (uint32_t)n;
  }
  return (int)got;
}

bool rar_path_is_safe(const char *name)
{
  if (!name || !*name || name[0] == '/' || name[0] == '\\')
    return false;
  if (((name[0] | 32) >= 'a' && (name[0] | 32) <= 'z') && name[1] == ':')
    return false;
  const char *p = name;
  while (*p) {
    const char *e = p;
    while (*e && *e != '/' && *e != '\\')
      e++;
    if (e - p == 2 && p[0] == '.' && p[1] == '.')
      return false;
    p = *e ? e + 1 : e;
  }
  return true;
}

static void free_decoders(struct rar *r)
{
  lzw_free(r);
  rar4_free(r);
  br_free(&r->br);
  r->br_ready = false;
}

void rar_close(struct rar *r)
{
  if (!r)
    return;
  free_decoders(r);
  rar_file_free(&r->cur);
  rar_vol_close(&r->hv);
  rar_vol_close(&r->dv);
  free(r->first_name);
  free(r->password);
  free(r->missing_name);
  free(r->hbuf);
  free(r);
}

static char *dup_str(const char *s)
{
  if (!s)
    return NULL;
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

int rar_set_password(struct rar *r, const char *password)
{
  char *p = dup_str(password);
  if (password && !p)
    return RAR_ENOMEM;
  free(r->password);
  r->password = p;
  r->kc.valid = false;
  r->hkc.valid = false;
  return RAR_OK;
}

int rar_open(struct rar **out, const struct rar_src *src, const struct rar_cfg *cfg)
{
  *out = NULL;
  struct rar *r = calloc(1, sizeof *r);
  if (!r) {
    if (src->close)
      src->close(src->ctx);
    return RAR_ENOMEM;
  }
  r->cfg = *cfg;
  r->hv.src = *src;
  r->hv.open = true;
  r->max_mem = cfg->max_mem ? cfg->max_mem : RAR_DEFAULT_MEM;
  r->list_only = cfg->list_only;
  r->first_name = dup_str(cfg->name);
  r->password = dup_str(cfg->password);
  if ((cfg->name && !r->first_name) || (cfg->password && !r->password)) {
    rar_close(r);
    return RAR_ENOMEM;
  }
  uint64_t end;
  int rc = rar_find_sig(r, &r->hv, &end, &r->fmt);
  if (rc == RAR_OK) {
    r->hoff = end;
    struct fh fh;
    rc = rar_scan(r, true, &fh);
    if (rc == 2) {
      rar_file_free(&fh.f);
      r->hoff = end;
      r->hdr_enc = false;
      r->main_seen = false;
    }
    if (rc >= 0)
      rc = r->hdr_enc && !r->password ? RAR_EPASSWORD : RAR_OK;
  }
  if (rc) {
    rar_close(r);
    return rc;
  }
  *out = r;
  return RAR_OK;
}

bool rar_is_solid(const struct rar *r) { return r->solid; }
bool rar_is_multivolume(const struct rar *r) { return r->multivol; }
bool rar_headers_encrypted(const struct rar *r) { return r->hdr_enc; }
int rar_format(const struct rar *r) { return r->fmt; }

/* ------------------------------------------------------------------ extraction */

static int run_decode(struct rar *r)
{
  struct rar_file *f = &r->cur;
  int rc;
  if (!r->br_ready) {
    rc = br_init(&r->br, r);
    if (rc)
      return rc;
    r->br_ready = true;
  }
  rc = pk_start(r);
  if (rc)
    return rc;
  if (f->method == 0) {
    if (!r->iobuf) {
      r->iobuf = malloc(0x10000);
      if (!r->iobuf)
        return RAR_ENOMEM;
    }
    uint64_t left = f->size;
    while (left) {
      uint32_t want = left > 0x10000 ? 0x10000 : (uint32_t)left;
      int n = pk_read(r, r->iobuf, want);
      if (n < 0)
        return n;
      if (n == 0)
        return RAR_ECORRUPT;
      rc = out_emit(r, r->iobuf, (uint32_t)n);
      if (rc)
        return rc;
      left -= (uint64_t)n;
    }
    return RAR_OK;
  }
  if (f->version < 29 && r->fmt == 4)
    return RAR_EOLD;
  if (f->version > 36 && r->fmt == 4)
    return RAR_EUNSUPPORTED;
  return r->fmt == 5 ? rar5_decode(r) : rar4_decode(r);
}

static int verify(struct rar *r)
{
  struct rar_file *f = &r->cur;
  struct rar_out *o = &r->out;
  if (!f->size_unknown && o->produced != f->size)
    return RAR_ECORRUPT;
  if (f->has_crc) {
    uint32_t got = o->crc;
    if (f->hash_mac) {
      uint8_t raw[4] = {(uint8_t)got, (uint8_t)(got >> 8), (uint8_t)(got >> 16), (uint8_t)(got >> 24)}, d[32];
      rar5_hash_mac(f->hashkey, raw, 4, d);
      got = 0;
      for (int i = 0; i < 32; i++)
        got ^= (uint32_t)d[i] << ((i & 3) * 8);
    }
    if (got != f->crc)
      return RAR_ECRC;
  }
  if (f->has_b2) {
    uint8_t d[32];
    b2sp_final(&o->b2, d);
    if (f->hash_mac) {
      uint8_t m[32];
      rar5_hash_mac(f->hashkey, d, 32, m);
      memcpy(d, m, 32);
    }
    if (memcmp(d, f->b2, 32) != 0)
      return RAR_ECRC;
  }
  return RAR_OK;
}

static int null_write(void *ctx, const void *buf, uint32_t n)
{
  (void)ctx;
  (void)buf;
  (void)n;
  return 0;
}

static int extract_current(struct rar *r, int (*write)(void *, const void *, uint32_t), void *ctx)
{
  struct rar_file *f = &r->cur;
  struct rar_out *o = &r->out;
  if (f->is_dir || f->is_hardlink || (f->is_symlink && r->fmt == 5))
    return RAR_OK;
  if (f->incomplete)
    return RAR_EVOLUME;
  if (f->size == 0 && f->method == 0 && !f->size_unknown && !f->has_crc)
    return RAR_OK;
  if (r->list_only)
    return RAR_ESTATE;
  memset(o, 0, sizeof *o);
  o->write = write;
  o->ctx = ctx;
  o->discard = write == null_write;
  o->use_b2 = f->has_b2;
  if (f->has_b2)
    b2sp_init(&o->b2);
  int rc = run_decode(r);
  if (rc == RAR_OK)
    rc = verify(r);
  if (rc && f->encrypted && r->fmt == 4 && (rc == RAR_ECORRUPT || rc == RAR_ECRC || rc == RAR_EUNSUPPORTED))
    rc = RAR_EPASSWORD;
  if (rc && f->method != 0)
    r->broken = true;
  return rc;
}

int rar_extract(struct rar *r, int (*write)(void *, const void *, uint32_t), void *ctx)
{
  if (!r->have_cur || r->cur_done)
    return RAR_ESTATE;
  int rc = extract_current(r, write, ctx);
  if (rc != RAR_EPASSWORD && rc != RAR_ESTATE)
    r->cur_done = true;
  return rc;
}

static int add_chunk(struct rar_file *f, const struct rar_chunk *c)
{
  if (f->nchunks == f->cchunks) {
    unsigned nc = f->cchunks ? f->cchunks * 2 : 4;
    struct rar_chunk *p = realloc(f->chunks, (size_t)nc * sizeof *p);
    if (!p)
      return RAR_ENOMEM;
    f->chunks = p;
    f->cchunks = nc;
  }
  f->chunks[f->nchunks++] = *c;
  return RAR_OK;
}

int rar_next(struct rar *r, struct rar_entry *e)
{
  if (r->vol_missing && !r->have_cur)
    return RAR_EVOLUME;
  if (r->have_cur && !r->cur_done && r->solid && !r->list_only && r->cur.method != 0 && !r->cur.incomplete &&
      !r->cur.is_dir && !r->cur.is_symlink && !r->cur.is_hardlink) {
    extract_current(r, null_write, NULL);
  }
  if (r->have_cur && r->cur.incomplete && r->vol_missing) {
    r->have_cur = false;
    rar_file_free(&r->cur);
    return RAR_EVOLUME;
  }
  rar_file_free(&r->cur);
  r->have_cur = false;
  struct fh fh;
  int rc = rar_scan(r, false, &fh);
  if (rc <= 0)
    return rc;
  if (fh.split_before) {
    rar_file_free(&fh.f);
    return RAR_EVOLUME;
  }
  fh.c.vol = r->hv.idx;
  fh.f.packed = fh.c.size;
  struct rar_file *f = &fh.f;
  rc = add_chunk(f, &fh.c);
  while (rc == RAR_OK && fh.split_after) {
    struct fh nx;
    f->split = true;
    rc = rar_next_volume(r);
    if (rc == RAR_EVOLUME) {
      f->incomplete = true;
      r->vol_missing = true;
      rc = RAR_OK;
      break;
    }
    if (rc)
      break;
    rc = rar_scan(r, false, &nx);
    if (rc == RAR_EVOLUME) {
      f->incomplete = true;
      r->vol_missing = true;
      rc = RAR_OK;
      break;
    }
    if (rc == 0 || (rc == 2 && (!nx.split_before || strcmp(nx.f.name, f->name) != 0))) {
      if (rc == 2)
        rar_file_free(&nx.f);
      rc = RAR_EFORMAT;
      break;
    }
    if (rc < 0)
      break;
    nx.c.vol = r->hv.idx;
    rc = add_chunk(f, &nx.c);
    f->packed += nx.c.size;
    f->has_crc = nx.f.has_crc;
    f->crc = nx.f.crc;
    f->has_b2 = nx.f.has_b2;
    memcpy(f->b2, nx.f.b2, 32);
    fh.split_after = nx.split_after;
    rar_file_free(&nx.f);
  }
  if (rc) {
    rar_file_free(f);
    return rc;
  }
  r->cur = *f;
  r->have_cur = true;
  r->cur_done = false;
  struct rar_file *c = &r->cur;
  memset(e, 0, sizeof *e);
  e->name = c->name;
  e->link = c->link;
  e->size = c->size_unknown ? UINT64_MAX : c->size;
  e->packed = c->packed;
  e->crc = c->crc;
  e->has_crc = c->has_crc;
  e->mtime = c->mtime;
  e->attr = c->attr;
  e->mode = c->mode;
  e->host = c->host;
  e->method = c->method;
  e->version = c->version;
  e->is_dir = c->is_dir;
  e->is_symlink = c->is_symlink;
  e->is_hardlink = c->is_hardlink;
  e->encrypted = c->encrypted;
  e->solid = c->solid;
  e->split = c->split;
  return 1;
}

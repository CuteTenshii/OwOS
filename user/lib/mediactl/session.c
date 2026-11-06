/* session.c: wire codec and session policy for mediad (see session.h). */
#include "session.h"
#include "owos.h"

static void bput(struct media_buf *b, const void *d, size_t n)
{
  if (b->oom)
    return;
  if (b->len + n > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 128;
    while (nc < b->len + n)
      nc *= 2;
    unsigned char *np = realloc(b->p, nc);
    if (!np) {
      b->oom = true;
      return;
    }
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->len, d, n);
  b->len += n;
}

static void put_u32(struct media_buf *b, unsigned v)
{
  unsigned char c[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
  bput(b, c, 4);
}

static size_t str_len(const char *s, size_t cap)
{
  size_t n = 0;
  while (n < cap && s[n])
    n++;
  return n;
}

static void put_str(struct media_buf *b, const char *s, size_t cap)
{
  size_t n = s ? str_len(s, cap - 1) : 0;
  unsigned char c[2] = {(unsigned char)n, (unsigned char)(n >> 8)};
  bput(b, c, 2);
  bput(b, s, n);
}

static void put_info(struct media_buf *b, const struct media_info *i)
{
  put_str(b, i->app, MEDIA_STR_APP);
  put_str(b, i->title, MEDIA_STR_TEXT);
  put_str(b, i->artist, MEDIA_STR_TEXT);
  put_str(b, i->album, MEDIA_STR_TEXT);
  put_str(b, i->artwork, MEDIA_STR_ART);
  put_u32(b, (unsigned)i->state);
  put_u32(b, i->pos_ms);
  put_u32(b, i->dur_ms);
  put_u32(b, i->caps);
}

static void begin(struct media_buf *b, int type)
{
  b->len = 0;
  b->oom = false;
  unsigned char t = (unsigned char)type;
  bput(b, &t, 1);
  put_u32(b, 0);
}

static bool end(struct media_buf *b)
{
  if (b->oom)
    return false;
  size_t n = b->len - MEDIA_HDR;
  for (int i = 0; i < 4; i++)
    b->p[1 + i] = (unsigned char)(n >> (8 * i));
  return true;
}

void media_buf_free(struct media_buf *b)
{
  free(b->p);
  memset(b, 0, sizeof *b);
}

bool media_enc_open(struct media_buf *b, unsigned pid, const char *app)
{
  begin(b, MEDIA_OP_OPEN);
  put_u32(b, pid);
  put_str(b, app, MEDIA_STR_APP);
  return end(b);
}

bool media_enc_update(struct media_buf *b, unsigned id, const struct media_info *info)
{
  begin(b, MEDIA_OP_UPDATE);
  put_u32(b, id);
  put_info(b, info);
  return end(b);
}

bool media_enc_id(struct media_buf *b, int op, unsigned id)
{
  begin(b, op);
  put_u32(b, id);
  return end(b);
}

bool media_enc_status(struct media_buf *b)
{
  begin(b, MEDIA_OP_STATUS);
  return end(b);
}

bool media_enc_send(struct media_buf *b, unsigned cmd, unsigned arg)
{
  begin(b, MEDIA_OP_SEND);
  put_u32(b, cmd);
  put_u32(b, arg);
  return end(b);
}

bool media_enc_ack(struct media_buf *b, bool ok)
{
  begin(b, ok ? MEDIA_ACK : MEDIA_NAK);
  return end(b);
}

bool media_enc_ack_id(struct media_buf *b, unsigned id)
{
  begin(b, MEDIA_ACK);
  put_u32(b, id);
  return end(b);
}

bool media_enc_ack_cmd(struct media_buf *b, unsigned cmd, unsigned arg)
{
  begin(b, MEDIA_ACK);
  put_u32(b, cmd);
  put_u32(b, arg);
  return end(b);
}

bool media_enc_ack_status(struct media_buf *b, const struct media_info *info)
{
  begin(b, MEDIA_ACK);
  unsigned char have = info != NULL;
  bput(b, &have, 1);
  if (info)
    put_info(b, info);
  return end(b);
}

bool media_hdr_parse(const unsigned char hdr[MEDIA_HDR], int *type, size_t *len)
{
  size_t n = (size_t)hdr[1] | (size_t)hdr[2] << 8 | (size_t)hdr[3] << 16 | (size_t)hdr[4] << 24;
  if (n > MEDIA_FRAME_MAX)
    return false;
  *type = hdr[0];
  *len = n;
  return true;
}

struct rd {
  const unsigned char *p;
  size_t len, pos;
  bool bad;
};

static unsigned get_u32(struct rd *r)
{
  if (r->bad || r->len - r->pos < 4) {
    r->bad = true;
    return 0;
  }
  const unsigned char *c = r->p + r->pos;
  r->pos += 4;
  return (unsigned)c[0] | (unsigned)c[1] << 8 | (unsigned)c[2] << 16 | (unsigned)c[3] << 24;
}

static void get_str(struct rd *r, char *dst, size_t cap)
{
  if (r->bad || r->len - r->pos < 2) {
    r->bad = true;
    return;
  }
  size_t n = (size_t)r->p[r->pos] | (size_t)r->p[r->pos + 1] << 8;
  r->pos += 2;
  if (n > cap - 1 || n > r->len - r->pos || str_len((const char *)r->p + r->pos, n) != n) {
    r->bad = true;
    return;
  }
  memcpy(dst, r->p + r->pos, n);
  dst[n] = '\0';
  r->pos += n;
}

static void get_info(struct rd *r, struct media_info *i)
{
  memset(i, 0, sizeof *i);
  get_str(r, i->app, MEDIA_STR_APP);
  get_str(r, i->title, MEDIA_STR_TEXT);
  get_str(r, i->artist, MEDIA_STR_TEXT);
  get_str(r, i->album, MEDIA_STR_TEXT);
  get_str(r, i->artwork, MEDIA_STR_ART);
  unsigned st = get_u32(r);
  i->pos_ms = get_u32(r);
  i->dur_ms = get_u32(r);
  i->caps = get_u32(r);
  if (st > MEDIA_PAUSED)
    r->bad = true;
  i->state = (int)st;
}

static bool done(const struct rd *r) { return !r->bad && r->pos == r->len; }

bool media_dec_request(int op, const unsigned char *p, size_t len, struct media_req *out)
{
  struct rd r = {p, len, 0, false};
  memset(out, 0, sizeof *out);
  out->op = op;
  switch (op) {
  case MEDIA_OP_OPEN:
    out->pid = get_u32(&r);
    get_str(&r, out->info.app, MEDIA_STR_APP);
    break;
  case MEDIA_OP_UPDATE:
    out->id = get_u32(&r);
    get_info(&r, &out->info);
    break;
  case MEDIA_OP_POLL:
  case MEDIA_OP_CLOSE:
    out->id = get_u32(&r);
    break;
  case MEDIA_OP_STATUS:
    break;
  case MEDIA_OP_SEND:
    out->cmd = get_u32(&r);
    out->arg = get_u32(&r);
    break;
  default:
    return false;
  }
  return done(&r);
}

bool media_dec_id(const unsigned char *p, size_t len, unsigned *id)
{
  struct rd r = {p, len, 0, false};
  *id = get_u32(&r);
  return done(&r);
}

bool media_dec_cmd(const unsigned char *p, size_t len, unsigned *cmd, unsigned *arg)
{
  struct rd r = {p, len, 0, false};
  *cmd = get_u32(&r);
  *arg = get_u32(&r);
  return done(&r);
}

bool media_dec_status(const unsigned char *p, size_t len, bool *have, struct media_info *info)
{
  if (len < 1 || p[0] > 1)
    return false;
  *have = p[0];
  if (!*have)
    return len == 1;
  struct rd r = {p, len, 1, false};
  get_info(&r, info);
  return done(&r);
}

void media_hub_free(struct media_hub *h)
{
  for (size_t i = 0; i < h->n; i++)
    free(h->s[i].q);
  free(h->s);
  memset(h, 0, sizeof *h);
}

static struct media_session *find(struct media_hub *h, unsigned id)
{
  for (size_t i = 0; i < h->n; i++)
    if (h->s[i].id == id)
      return &h->s[i];
  return NULL;
}

int media_hub_open(struct media_hub *h, unsigned pid, const char *app)
{
  if (h->n == h->cap) {
    size_t nc = h->cap ? h->cap * 2 : 4;
    struct media_session *ns = realloc(h->s, nc * sizeof *ns);
    if (!ns)
      return -1;
    h->s = ns;
    h->cap = nc;
  }
  struct media_session *s = &h->s[h->n++];
  memset(s, 0, sizeof *s);
  s->id = ++h->next_id;
  s->pid = pid;
  s->updated = ++h->tick;
  if (app)
    memcpy(s->info.app, app, str_len(app, MEDIA_STR_APP - 1));
  return (int)s->id;
}

int media_hub_update(struct media_hub *h, unsigned id, const struct media_info *info)
{
  struct media_session *s = find(h, id);
  if (!s || info->state < MEDIA_STOPPED || info->state > MEDIA_PAUSED)
    return -1;
  if (info->state == MEDIA_PLAYING && s->info.state != MEDIA_PLAYING)
    s->played = ++h->tick;
  s->updated = ++h->tick;
  s->info = *info;
  s->info.app[MEDIA_STR_APP - 1] = s->info.title[MEDIA_STR_TEXT - 1] = '\0';
  s->info.artist[MEDIA_STR_TEXT - 1] = s->info.album[MEDIA_STR_TEXT - 1] = '\0';
  s->info.artwork[MEDIA_STR_ART - 1] = '\0';
  return 0;
}

int media_hub_close(struct media_hub *h, unsigned id)
{
  struct media_session *s = find(h, id);
  if (!s)
    return -1;
  free(s->q);
  *s = h->s[--h->n];
  return 0;
}

int media_hub_poll(struct media_hub *h, unsigned id, unsigned *arg)
{
  struct media_session *s = find(h, id);
  if (!s)
    return -1;
  if (!s->qn) {
    *arg = 0;
    return MEDIA_CMD_NONE;
  }
  int cmd = (int)s->q[0].cmd;
  *arg = s->q[0].arg;
  memmove(s->q, s->q + 1, --s->qn * sizeof *s->q);
  return cmd;
}

struct media_session *media_hub_active(struct media_hub *h)
{
  struct media_session *best = NULL;
  for (size_t i = 0; i < h->n; i++) {
    struct media_session *s = &h->s[i];
    if (!best) {
      best = s;
      continue;
    }
    bool sp = s->info.state == MEDIA_PLAYING, bp = best->info.state == MEDIA_PLAYING;
    if (sp != bp ? sp : sp ? s->played > best->played : s->updated > best->updated)
      best = s;
  }
  return best;
}

static bool is_seek(unsigned c)
{
  return c == MEDIA_CMD_SEEK_FWD || c == MEDIA_CMD_SEEK_BACK || c == MEDIA_CMD_SEEK_TO;
}

/* Merge a command into the tail of the queue where order makes that safe; true if absorbed */
static bool coalesce(struct media_session *s, unsigned cmd, unsigned arg)
{
  if (!s->qn)
    return false;
  unsigned lc = s->q[s->qn - 1].cmd;
  if (lc == cmd && (cmd == MEDIA_CMD_PLAY || cmd == MEDIA_CMD_PAUSE || cmd == MEDIA_CMD_STOP))
    return true;
  if (lc == MEDIA_CMD_TOGGLE && cmd == MEDIA_CMD_TOGGLE) {
    s->qn--;
    return true;
  }
  if (cmd == MEDIA_CMD_SEEK_TO) {
    while (s->qn && is_seek(s->q[s->qn - 1].cmd))
      s->qn--;
    return false;
  }
  if (lc == cmd && is_seek(cmd) && arg && s->q[s->qn - 1].arg) {
    unsigned sum = s->q[s->qn - 1].arg + arg;
    s->q[s->qn - 1].arg = sum < arg ? ~0u : sum;
    return true;
  }
  return false;
}

int media_hub_send(struct media_hub *h, unsigned cmd, unsigned arg)
{
  struct media_session *s = media_hub_active(h);
  if (!s || cmd <= MEDIA_CMD_NONE || cmd > MEDIA_CMD_SEEK_TO)
    return -1;
  if (coalesce(s, cmd, arg))
    return 0;
  if (s->qn == s->qcap) {
    size_t nc = s->qcap ? s->qcap * 2 : 4;
    void *nq = realloc(s->q, nc * sizeof *s->q);
    if (!nq)
      return -1;
    s->q = nq;
    s->qcap = nc;
  }
  s->q[s->qn].cmd = cmd;
  s->q[s->qn++].arg = arg;
  return 0;
}

size_t media_hub_reap(struct media_hub *h, media_alive_fn alive, void *ctx)
{
  size_t reaped = 0;
  for (size_t i = 0; i < h->n;) {
    if (alive(h->s[i].pid, ctx)) {
      i++;
      continue;
    }
    free(h->s[i].q);
    h->s[i] = h->s[--h->n];
    reaped++;
  }
  return reaped;
}

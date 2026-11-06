/* mediad.c, the media session daemon. Owns the "media" IPC port and keeps the session list;
 * the wire protocol and the session policy live in user/lib/mediactl/session.c. */
#include "owos.h"
#include "session.h"

static struct media_hub hub;

static bool pid_alive(unsigned pid, void *ctx)
{
  (void)ctx;
  return kill((int)pid, 0) == 0;
}

static bool read_full(int fd, void *buf, size_t n)
{
  unsigned char *p = buf;
  for (size_t got = 0; got < n;) {
    int r = read(fd, p + got, (int)(n - got));
    if (r <= 0)
      return false;
    got += (size_t)r;
  }
  return true;
}

static bool handle(const struct media_req *rq, struct media_buf *out)
{
  unsigned arg;
  int r;
  switch (rq->op) {
  case MEDIA_OP_OPEN:
    r = media_hub_open(&hub, rq->pid, rq->info.app);
    return r < 0 ? media_enc_ack(out, false) : media_enc_ack_id(out, (unsigned)r);
  case MEDIA_OP_UPDATE:
    return media_enc_ack(out, media_hub_update(&hub, rq->id, &rq->info) == 0);
  case MEDIA_OP_POLL:
    r = media_hub_poll(&hub, rq->id, &arg);
    return r < 0 ? media_enc_ack(out, false) : media_enc_ack_cmd(out, (unsigned)r, arg);
  case MEDIA_OP_CLOSE:
    return media_enc_ack(out, media_hub_close(&hub, rq->id) == 0);
  case MEDIA_OP_STATUS: {
    struct media_session *s = media_hub_active(&hub);
    return media_enc_ack_status(out, s ? &s->info : NULL);
  }
  default:
    return media_enc_ack(out, media_hub_send(&hub, rq->cmd, rq->arg) == 0);
  }
}

static void serve(int ch)
{
  unsigned char hdr[MEDIA_HDR];
  int op;
  size_t len;
  if (!read_full(ch, hdr, MEDIA_HDR) || !media_hdr_parse(hdr, &op, &len))
    return;
  unsigned char *payload = len ? malloc((unsigned)len) : NULL;
  struct media_req rq;
  struct media_buf out = {0};
  bool ok = (!len || (payload && read_full(ch, payload, len))) && media_dec_request(op, payload, len, &rq);
  free(payload);
  media_hub_reap(&hub, pid_alive, NULL);
  if (!(ok ? handle(&rq, &out) : media_enc_ack(&out, false))) {
    media_buf_free(&out);
    return;
  }
  size_t put = 0;
  while (put < out.len) {
    int w = write(ch, out.p + put, (int)(out.len - put));
    if (w <= 0)
      break;
    put += (size_t)w;
  }
  media_buf_free(&out);
}

int main(void)
{
  int lst = port_open(MEDIA_PORT, 1);
  if (lst < 0)
    return 1;
  for (;;) {
    int ch = port_accept(lst);
    if (ch < 0)
      continue;
    serve(ch);
    close(ch);
  }
}

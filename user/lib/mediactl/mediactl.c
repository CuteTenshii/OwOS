/* mediactl.c: client side of the mediad protocol (see session.h). One short connection per call. */
#include "mediactl.h"
#include "owos.h"
#include "session.h"

static bool io_full(int fd, void *buf, size_t n, bool wr)
{
  unsigned char *p = buf;
  for (size_t done = 0; done < n;) {
    int r = wr ? write(fd, p + done, (int)(n - done)) : read(fd, p + done, (int)(n - done));
    if (r <= 0)
      return false;
    done += (size_t)r;
  }
  return true;
}

/* Send the frame and read the reply; *payload is malloc'd (caller frees). 0 on ACK, -1 otherwise */
static int call(struct media_buf *req, unsigned char **payload, size_t *plen)
{
  *payload = NULL;
  *plen = 0;
  int fd = port_open(MEDIA_PORT, 0);
  if (fd < 0)
    return -1;
  unsigned char hdr[MEDIA_HDR];
  int type = 0;
  size_t len = 0;
  bool ok = io_full(fd, req->p, req->len, true) && io_full(fd, hdr, MEDIA_HDR, false) &&
            media_hdr_parse(hdr, &type, &len) && type == MEDIA_ACK;
  unsigned char *p = NULL;
  if (ok && len) {
    p = malloc((unsigned)len);
    ok = p && io_full(fd, p, len, false);
  }
  close(fd);
  if (!ok) {
    free(p);
    return -1;
  }
  *payload = p;
  *plen = len;
  return 0;
}

static int call_plain(struct media_buf *req)
{
  unsigned char *p;
  size_t n;
  int r = call(req, &p, &n);
  free(p);
  media_buf_free(req);
  return r == 0 && n == 0 ? 0 : -1;
}

int mediactl_open(const char *app)
{
  struct media_buf b = {0};
  unsigned char *p;
  size_t n;
  unsigned id;
  if (!media_enc_open(&b, (unsigned)getpid(), app)) {
    media_buf_free(&b);
    return -1;
  }
  int r = call(&b, &p, &n);
  media_buf_free(&b);
  bool ok = r == 0 && media_dec_id(p, n, &id);
  free(p);
  return ok ? (int)id : -1;
}

int mediactl_update(int id, const struct media_info *info)
{
  struct media_buf b = {0};
  if (id < 0 || !media_enc_update(&b, (unsigned)id, info)) {
    media_buf_free(&b);
    return -1;
  }
  return call_plain(&b);
}

int mediactl_poll(int id, unsigned *arg)
{
  struct media_buf b = {0};
  unsigned a = 0, cmd = MEDIA_CMD_NONE;
  unsigned char *p = NULL;
  size_t n = 0;
  int r = -1;
  if (id >= 0 && media_enc_id(&b, MEDIA_OP_POLL, (unsigned)id))
    r = call(&b, &p, &n);
  media_buf_free(&b);
  bool ok = r == 0 && media_dec_cmd(p, n, &cmd, &a);
  free(p);
  if (arg)
    *arg = ok ? a : 0;
  return ok ? (int)cmd : MEDIA_CMD_NONE;
}

int mediactl_close(int id)
{
  struct media_buf b = {0};
  if (id < 0 || !media_enc_id(&b, MEDIA_OP_CLOSE, (unsigned)id)) {
    media_buf_free(&b);
    return -1;
  }
  return call_plain(&b);
}

int mediactl_status(struct media_info *out)
{
  struct media_buf b = {0};
  unsigned char *p;
  size_t n;
  bool have = false;
  struct media_info tmp;
  int r = -1;
  if (media_enc_status(&b))
    r = call(&b, &p, &n);
  media_buf_free(&b);
  if (r < 0)
    return -1;
  bool ok = media_dec_status(p, n, &have, &tmp);
  free(p);
  if (!ok)
    return -1;
  if (have && out)
    *out = tmp;
  return have ? 1 : 0;
}

int mediactl_send(int cmd, unsigned arg)
{
  struct media_buf b = {0};
  if (cmd <= MEDIA_CMD_NONE || !media_enc_send(&b, (unsigned)cmd, arg)) {
    media_buf_free(&b);
    return -1;
  }
  return call_plain(&b);
}

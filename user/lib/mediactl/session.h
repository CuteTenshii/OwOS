/* session.h: wire codec and session policy for mediad, free of syscalls so the host test links it.
 *
 * Wire protocol over the "media" port, one short connection per request. Every frame, request
 * or reply, is: type byte, u32 LE payload length, payload. Integers are u32 LE; strings are u16 LE
 * length + bytes (no NUL), at most the field's buffer size minus one.
 *
 *   request  payload                                   reply (MEDIA_ACK, or MEDIA_NAK + empty)
 *   'O' open    pid, app                               id
 *   'U' update  id, app, title, artist, album,         empty
 *               artwork, state, pos, dur, caps
 *   'P' poll    id                                     cmd, arg
 *   'C' close   id                                     empty
 *   'S' status  empty                                  have (u8), then update's fields after id when have
 *   'D' send    cmd, arg                               empty (NAK when there is no active session)
 */
#ifndef OWOS_MEDIA_SESSION_H
#define OWOS_MEDIA_SESSION_H

#include "mediactl.h"
#include <stdbool.h>
#include <stddef.h>

#define MEDIA_ACK 'K'
#define MEDIA_NAK 'E'
#define MEDIA_OP_OPEN 'O'
#define MEDIA_OP_UPDATE 'U'
#define MEDIA_OP_POLL 'P'
#define MEDIA_OP_CLOSE 'C'
#define MEDIA_OP_STATUS 'S'
#define MEDIA_OP_SEND 'D'

#define MEDIA_HDR 5
#define MEDIA_FRAME_MAX 4096

struct media_buf {
  unsigned char *p;
  size_t len, cap;
  bool oom;
};

struct media_req {
  int op;
  unsigned id, pid, cmd, arg;
  struct media_info info;
};

void media_buf_free(struct media_buf *b);

/* Frame encoders: reset *b and fill it with one complete frame; false on allocation failure */
bool media_enc_open(struct media_buf *b, unsigned pid, const char *app);
bool media_enc_update(struct media_buf *b, unsigned id, const struct media_info *info);
bool media_enc_id(struct media_buf *b, int op, unsigned id); /* poll, close */
bool media_enc_status(struct media_buf *b);
bool media_enc_send(struct media_buf *b, unsigned cmd, unsigned arg);

bool media_enc_ack(struct media_buf *b, bool ok);
bool media_enc_ack_id(struct media_buf *b, unsigned id);
bool media_enc_ack_cmd(struct media_buf *b, unsigned cmd, unsigned arg);
bool media_enc_ack_status(struct media_buf *b, const struct media_info *info); /* NULL: none */

/* Header parse: false when the length exceeds MEDIA_FRAME_MAX */
bool media_hdr_parse(const unsigned char hdr[MEDIA_HDR], int *type, size_t *len);

/* Payload decoders: false on truncation, oversized or malformed fields, trailing bytes */
bool media_dec_request(int op, const unsigned char *p, size_t len, struct media_req *out);
bool media_dec_id(const unsigned char *p, size_t len, unsigned *id);
bool media_dec_cmd(const unsigned char *p, size_t len, unsigned *cmd, unsigned *arg);
bool media_dec_status(const unsigned char *p, size_t len, bool *have, struct media_info *info);

/* Session policy */
struct media_session {
  unsigned id, pid;
  struct media_info info;
  unsigned long long played, updated; /* hub tick of the last start of playback / last update */
  struct {
    unsigned cmd, arg;
  } *q;
  size_t qn, qcap;
};

struct media_hub {
  struct media_session *s;
  size_t n, cap;
  unsigned next_id;
  unsigned long long tick;
};

typedef bool (*media_alive_fn)(unsigned pid, void *ctx);

void media_hub_free(struct media_hub *h);
int media_hub_open(struct media_hub *h, unsigned pid, const char *app); /* id, or -1 */
int media_hub_update(struct media_hub *h, unsigned id, const struct media_info *info);
int media_hub_close(struct media_hub *h, unsigned id);
int media_hub_poll(struct media_hub *h, unsigned id, unsigned *arg); /* cmd; -1 unknown session */
struct media_session *media_hub_active(struct media_hub *h);
int media_hub_send(struct media_hub *h, unsigned cmd, unsigned arg); /* 0, or -1 none/invalid */
size_t media_hub_reap(struct media_hub *h, media_alive_fn alive, void *ctx);

#endif

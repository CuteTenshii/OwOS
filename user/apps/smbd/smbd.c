/* smbd.c — a minimal read-only SMB2 server, in user space.
 *
 * Ported from the former in-kernel kernel/smb.c: the SMB2 wire protocol is a
 * pure byte-stream server over TCP/445 plus file reads, so it needs no ring-0
 * privilege and lives here as a windowless /bin daemon. main() opens a listener
 * and loops accept -> serve -> close; run it detached (e.g. `smbd &`).
 *
 * Scope and shape: one client per connection, dialect 2.0.2 only, guest
 * authentication (no password check), a single hard-coded read-only share
 * ("owofs") backed by owofs. It implements just the command set a stock client
 * walks through to browse and read files:
 *
 *   NEGOTIATE -> SESSION_SETUP (NTLMSSP, granted as guest) -> TREE_CONNECT
 *   -> CREATE -> QUERY_DIRECTORY / QUERY_INFO / READ -> CLOSE -> ...
 *
 * Everything is little-endian on the wire (SMB2), framed by the NetBIOS
 * session header (a 4-byte big-endian length) on direct TCP/445. The server is
 * synchronous: serve() reads one request, builds one response, writes it, and
 * repeats — blocking in net_recv is fine.
 *
 * The certificate-equivalent caveat: authentication is cosmetic. The server
 * completes the NTLMSSP handshake but accepts ANY credentials as guest and
 * never verifies them. Good for serving files to yourself over QEMU's NAT, not
 * a security boundary.
 */

#include "owos.h"
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------- LE accessors */

static uint16_t load_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t load_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t load_le64(const uint8_t *p) { return (uint64_t)load_le32(p) | ((uint64_t)load_le32(p + 4) << 32); }

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = v;
  p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}
static void put64(uint8_t *p, uint64_t v)
{
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------- SMB2 on wire */

#define SMB2_MAGIC0 0xFE /* 0xFE 'S' 'M' 'B' */
#define SMB2_HDR 64

enum {
  SMB2_NEGOTIATE = 0x0000,
  SMB2_SESSION_SETUP = 0x0001,
  SMB2_LOGOFF = 0x0002,
  SMB2_TREE_CONNECT = 0x0003,
  SMB2_TREE_DISCONNECT = 0x0004,
  SMB2_CREATE = 0x0005,
  SMB2_CLOSE = 0x0006,
  SMB2_READ = 0x0008,
  SMB2_QUERY_DIRECTORY = 0x000E,
  SMB2_QUERY_INFO = 0x0010,
};

#define SMB2_FLAGS_RESPONSE 0x00000001

/* NTSTATUS values we use. */
#define STATUS_SUCCESS 0x00000000u
#define STATUS_MORE_PROCESSING                                                                                         \
  0xC0000016u /* STATUS_MORE_PROCESSING_REQUIRED                                                                       \
               */
#define STATUS_NOT_SUPPORTED 0xC00000BBu
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_OBJECT_NAME_NOTFOUND 0xC0000034u

#define SMB2_DIALECT_0202 0x0202

/* SPNEGO NegTokenInit advertising NTLMSSP — the security blob the server emits
 * in NEGOTIATE so the client knows to drive NTLMSSP in SESSION_SETUP. */
static const uint8_t spnego_neg_init[] = {
    0x60, 0x1c, 0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02, 0xa0, 0x12, 0x30, 0x10, 0xa0,
    0x0e, 0x30, 0x0c, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a,
};

/* --------------------------------------------------------------- buffers/IO */

#define SMB_MSG_MAX 70000u /* per-message cap (NBT length is 24 bits) */
#define SMB_TREE_ID 1      /* the single share's tree id              */
#define SMB_MAX_HANDLES 16

/* An open "file": a root-directory handle (for QUERY_DIRECTORY) or a regular
 * file (for READ). One client at a time, so a tiny fixed table suffices. The
 * SMB2 FileId's first 8 bytes carry the slot index; the rest is a fixed magic.
 */
struct smb_handle {
  bool used;
  bool is_dir;
  char path[128]; /* owofs path, e.g. "/bin/hello"; "/" = root */
  uint32_t size;
  uint32_t mtime; /* owofs packed mtime (0 = unknown) */
  int enum_pos;   /* QUERY_DIRECTORY cursor into the listing */
};

struct smb_conn {
  int fd;
  uint8_t rx[SMB_MSG_MAX];
  uint8_t tx[SMB_MSG_MAX];
  uint64_t session_id;
  struct smb_handle handles[SMB_MAX_HANDLES];
  bool closed;
};

/* Read exactly n bytes into buf; returns 0 on success, -1 on EOF/error. */
static int read_full(struct smb_conn *s, uint8_t *buf, uint32_t n)
{
  uint32_t got = 0;
  while (got < n) {
    int r = net_recv(s->fd, buf + got, (int)(n - got));
    if (r <= 0) /* 0 = EOF, <0 = error/interrupted (NET_EINTR) */
      return -1;
    got += (uint32_t)r;
  }
  return 0;
}

/* Read one NetBIOS-framed SMB2 message into s->rx; returns body length or -1.
 */
static int read_msg(struct smb_conn *s)
{
  uint8_t hdr[4];
  if (read_full(s, hdr, 4) < 0)
    return -1;
  /* hdr[0] is the NBT message type (0x00 = session message); length is the
   * low 24 bits, big-endian. */
  uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
  if (len < SMB2_HDR || len > SMB_MSG_MAX)
    return -1;
  if (read_full(s, s->rx, len) < 0)
    return -1;
  return (int)len;
}

/* Write all of buf[0..n); returns 0 on success, -1 on error. */
static int write_full(struct smb_conn *s, const uint8_t *buf, uint32_t n)
{
  uint32_t sent = 0;
  while (sent < n) {
    int r = net_send(s->fd, buf + sent, (int)(n - sent));
    if (r <= 0)
      return -1;
    sent += (uint32_t)r;
  }
  return 0;
}

/* Send body[0..len) wrapped in a NetBIOS session header. */
static int send_msg(struct smb_conn *s, const uint8_t *body, uint32_t len)
{
  uint8_t nbt[4];
  nbt[0] = 0x00;
  nbt[1] = (uint8_t)(len >> 16);
  nbt[2] = (uint8_t)(len >> 8);
  nbt[3] = (uint8_t)len;
  if (write_full(s, nbt, 4) < 0)
    return -1;
  if (write_full(s, body, len) < 0)
    return -1;
  return 0;
}

/* Fill the 64-byte SMB2 response header in s->tx, echoing the request's
 * command, MessageId, SessionId and TreeId, with the RESPONSE flag and given
 * status. */
static void build_header(struct smb_conn *s, const uint8_t *req, uint32_t status)
{
  uint8_t *h = s->tx;
  for (int i = 0; i < SMB2_HDR; i++)
    h[i] = 0;
  h[0] = SMB2_MAGIC0;
  h[1] = 'S';
  h[2] = 'M';
  h[3] = 'B';
  put16(h + 4, SMB2_HDR);             /* StructureSize */
  put16(h + 6, load_le16(req + 6));   /* CreditCharge */
  put32(h + 8, status);               /* Status */
  put16(h + 12, load_le16(req + 12)); /* Command */
  put16(h + 14, 1);                   /* CreditResponse: grant 1 */
  put32(h + 16, SMB2_FLAGS_RESPONSE); /* Flags */
  put32(h + 20, 0);                   /* NextCommand */
  put64(h + 24, load_le64(req + 24)); /* MessageId */
  put32(h + 32, 0);                   /* Reserved */
  put32(h + 36, load_le32(req + 36)); /* TreeId */
  put64(h + 40, s->session_id);       /* SessionId */
                                      /* Signature (16) left zero. */
}

/* Fallback Windows FILETIME (100ns ticks since 1601, ~2015) for entries with no
 * recorded time (owofs mtime == 0, e.g. never stamped or a generated pseudo-fs
 * node). Real entries use their converted owofs stamp (see owofs_filetime). */
#define FAKE_FILETIME 0x01D0000000000000ull

/* Convert an owofs packed mtime ([year-2000:6][month:4][day:5][hour:5][min:6]
 * [sec:6]) to a Windows FILETIME. Returns FAKE_FILETIME if unset (m == 0). */
static uint64_t owofs_filetime(uint32_t m)
{
  if (m == 0)
    return FAKE_FILETIME;
  int year = 2000 + (int)(m >> 26);
  int mon = (int)((m >> 22) & 0xF);
  int day = (int)((m >> 17) & 0x1F);
  int hour = (int)((m >> 12) & 0x1F);
  int min = (int)((m >> 6) & 0x3F);
  int sec = (int)(m & 0x3F);
  if (mon < 1)
    mon = 1;
  if (day < 1)
    day = 1;
  /* days since 1970-01-01 (Howard Hinnant's days_from_civil) */
  int y = year - (mon <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int days = era * 146097 + doe - 719468;
  uint64_t secs = (uint64_t)days * 86400 + (uint64_t)hour * 3600 + (uint64_t)min * 60 + (uint64_t)sec;
  /* shift epoch 1970 -> 1601 (11644473600 s) and scale to 100ns ticks */
  return (secs + 11644473600ull) * 10000000ull;
}

/* ------------------------------------------------------------- NEGOTIATE */

static int do_negotiate(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  (void)req;
  (void)reqlen;
  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;   /* response body */
  uint32_t secoff = SMB2_HDR + 64; /* security buffer right after body */

  put16(b + 0, 65);                /* StructureSize (fixed, incl 1 buf byte) */
  put16(b + 2, 0x0001);            /* SecurityMode: signing enabled */
  put16(b + 4, SMB2_DIALECT_0202); /* DialectRevision */
  put16(b + 6, 0);                 /* NegotiateContextCount/Reserved */
  for (int i = 0; i < 16; i++)
    b[8 + i] = (uint8_t)(0x11 * (i + 1)); /* ServerGuid */
  put32(b + 24, 0);                       /* Capabilities */
  put32(b + 28, 65536);                   /* MaxTransactSize */
  put32(b + 32, 65536);                   /* MaxReadSize */
  put32(b + 36, 65536);                   /* MaxWriteSize */
  put64(b + 40, FAKE_FILETIME);           /* SystemTime */
  put64(b + 48, 0);                       /* ServerStartTime */
  put16(b + 56, (uint16_t)secoff);        /* SecurityBufferOffset (from msg start) */
  put16(b + 58, (uint16_t)sizeof spnego_neg_init);
  put32(b + 60, 0); /* NegotiateContextOffset/Reserved2 */

  for (uint32_t i = 0; i < sizeof spnego_neg_init; i++)
    s->tx[secoff + i] = spnego_neg_init[i];

  uint32_t total = secoff + (uint32_t)sizeof spnego_neg_init;
  return send_msg(s, s->tx, total);
}

/* ------------------------------------------------------- SESSION_SETUP / NTLM
 *
 * The client drives NTLMSSP inside SPNEGO: it sends a NEGOTIATE (type 1), we
 * reply with a CHALLENGE (type 2) and STATUS_MORE_PROCESSING_REQUIRED, it sends
 * an AUTHENTICATE (type 3), and we accept it as guest without checking
 * anything. We don't parse SPNEGO/ASN.1 on the way in — we just scan for the
 * "NTLMSSP\0" signature and read the message type. On the way out we hand-build
 * the small SPNEGO wrappers (DER) since their shapes are fixed.
 */

#define SMB2_SESSION_FLAG_IS_GUEST 0x0001
#define OWOS_SESSION_ID 0x0000000000574F4Full /* "OW0" — any nonzero */

static const uint8_t ntlmssp_sig[8] = {'N', 'T', 'L', 'M', 'S', 'S', 'P', 0};

/* Find the NTLMSSP message inside a request; returns its start or NULL. */
static const uint8_t *find_ntlmssp(const uint8_t *buf, uint32_t len)
{
  if (len < 12)
    return NULL;
  for (uint32_t i = 0; i + 12 <= len; i++) {
    bool hit = true;
    for (int j = 0; j < 8; j++)
      if (buf[i + j] != ntlmssp_sig[j]) {
        hit = false;
        break;
      }
    if (hit)
      return buf + i;
  }
  return NULL;
}

/* DER length byte(s) for values < 256. Returns bytes written. */
static uint32_t put_der_len(uint8_t *p, uint32_t L)
{
  if (L < 128) {
    p[0] = (uint8_t)L;
    return 1;
  }
  p[0] = 0x81;
  p[1] = (uint8_t)L;
  return 2;
}

/* Append a UTF-16LE copy of an ASCII string; returns bytes written (2*len). */
static uint32_t put_utf16(uint8_t *p, const char *s)
{
  uint32_t n = 0;
  for (; *s; s++) {
    p[n++] = (uint8_t)*s;
    p[n++] = 0;
  }
  return n;
}

/* Build an NTLMSSP CHALLENGE (type 2) into out; returns its length. Fixed
 * server challenge, server-type target with a small target-info block — enough
 * for any client to form a (NTLMv1 or v2) AUTHENTICATE, which we then ignore.
 */
static uint32_t build_ntlm_challenge(uint8_t *out)
{
  for (int i = 0; i < 48; i++)
    out[i] = 0;
  for (int i = 0; i < 8; i++)
    out[i] = ntlmssp_sig[i];
  put32(out + 8, 2); /* MessageType = CHALLENGE */

  uint32_t pay = 48; /* payload starts after fixed header */
  uint32_t tnoff = pay;
  uint32_t tnlen = put_utf16(out + pay, "OWOS");
  pay += tnlen;
  uint32_t tioff = pay;
  /* TargetInfo AV pairs: NbComputerName, NbDomainName, EOL. */
  uint8_t *ti = out + pay;
  uint32_t t = 0;
  put16(ti + t, 1);
  t += 2;
  {
    uint32_t l = put_utf16(ti + t + 2, "OWOS");
    put16(ti + t, (uint16_t)l);
    t += 2 + l;
  }
  put16(ti + t, 2);
  t += 2;
  {
    uint32_t l = put_utf16(ti + t + 2, "OWOS");
    put16(ti + t, (uint16_t)l);
    t += 2 + l;
  }
  put16(ti + t, 0);
  t += 2;
  put16(ti + t, 0);
  t += 2; /* MsvAvEOL */
  uint32_t tilen = t;
  pay += tilen;

  put16(out + 12, (uint16_t)tnlen);       /* TargetNameLen */
  put16(out + 14, (uint16_t)tnlen);       /* TargetNameMaxLen */
  put32(out + 16, tnoff);                 /* TargetNameOffset */
  put32(out + 20, 0x008A8205);            /* NegotiateFlags */
  put64(out + 24, 0x0123456789ABCDEFull); /* ServerChallenge */
  /* out+32 Reserved (8) already zero */
  put16(out + 40, (uint16_t)tilen); /* TargetInfoLen */
  put16(out + 42, (uint16_t)tilen); /* TargetInfoMaxLen */
  put32(out + 44, tioff);           /* TargetInfoOffset */
  return pay;
}

/* Wrap an NTLMSSP message in an SPNEGO NegTokenResp
 * (negState=accept-incomplete, responseToken=<ntlm>). Returns total length. */
static uint32_t wrap_spnego_resp(uint8_t *out, const uint8_t *ntlm, uint32_t nlen)
{
  uint8_t inner[600];
  uint32_t p = 0;
  /* negState [0] = accept-incomplete (1) */
  inner[p++] = 0xA0;
  inner[p++] = 0x03;
  inner[p++] = 0x0A;
  inner[p++] = 0x01;
  inner[p++] = 0x01;
  /* responseToken [2] -> OCTET STRING -> ntlm */
  uint32_t octhdr = 1 + put_der_len((uint8_t[3]){0}, nlen); /* size of "04 LEN" */
  inner[p++] = 0xA2;
  p += put_der_len(inner + p, octhdr + nlen);
  inner[p++] = 0x04;
  p += put_der_len(inner + p, nlen);
  for (uint32_t i = 0; i < nlen; i++)
    inner[p++] = ntlm[i];

  uint32_t o = 0;
  out[o++] = 0xA1;
  o += put_der_len(out + o, p + 1 + put_der_len((uint8_t[3]){0}, p));
  out[o++] = 0x30;
  o += put_der_len(out + o, p);
  for (uint32_t i = 0; i < p; i++)
    out[o++] = inner[i];
  return o;
}

/* SPNEGO NegTokenResp negState=accept-completed (the final SUCCESS blob). */
static const uint8_t spnego_accept_done[] = {
    0xA1, 0x07, 0x30, 0x05, 0xA0, 0x03, 0x0A, 0x01, 0x00,
};

static int session_setup_response(struct smb_conn *s, const uint8_t *req, uint32_t status, uint16_t session_flags,
                                  const uint8_t *blob, uint32_t bloblen)
{
  build_header(s, req, status);
  uint8_t *b = s->tx + SMB2_HDR;
  uint32_t secoff = SMB2_HDR + 8;  /* body is 8 bytes + 1 pad before buf */
  put16(b + 0, 9);                 /* StructureSize (incl 1 buf byte) */
  put16(b + 2, session_flags);     /* SessionFlags */
  put16(b + 4, (uint16_t)secoff);  /* SecurityBufferOffset */
  put16(b + 6, (uint16_t)bloblen); /* SecurityBufferLength */
  for (uint32_t i = 0; i < bloblen; i++)
    s->tx[secoff + i] = blob[i];
  return send_msg(s, s->tx, secoff + bloblen);
}

static int do_session_setup(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  const uint8_t *ntlm = find_ntlmssp(req, reqlen);
  uint32_t type = ntlm ? load_le32(ntlm + 8) : 0;

  if (type == 1) { /* NEGOTIATE -> CHALLENGE */
    s->session_id = OWOS_SESSION_ID;
    uint8_t chal[256];
    uint32_t clen = build_ntlm_challenge(chal);
    uint8_t blob[600];
    uint32_t blen = wrap_spnego_resp(blob, chal, clen);
    return session_setup_response(s, req, STATUS_MORE_PROCESSING, 0, blob, blen);
  }
  /* type 3 AUTHENTICATE (or anything else): accept as guest. */
  s->session_id = OWOS_SESSION_ID;
  return session_setup_response(s, req, STATUS_SUCCESS, SMB2_SESSION_FLAG_IS_GUEST, spnego_accept_done,
                                sizeof spnego_accept_done);
}

/* Minimal error response: just the header with an error status and a 9-byte
 * SMB2 ERROR response body (StructureSize=9, the standard shape). */
static int do_error(struct smb_conn *s, const uint8_t *req, uint32_t status)
{
  build_header(s, req, status);
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 9); /* StructureSize */
  b[2] = 0;
  b[3] = 0;        /* ErrorContextCount / Reserved */
  put32(b + 4, 0); /* ByteCount */
  b[8] = 0;        /* one byte of (empty) error data */
  return send_msg(s, s->tx, SMB2_HDR + 9);
}

/* --------------------------------------------------------- TREE_CONNECT */

static int do_tree_connect(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  (void)reqlen;
  build_header(s, req, STATUS_SUCCESS);
  put32(s->tx + 36, SMB_TREE_ID); /* assign the share's TreeId */
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 16);          /* StructureSize */
  b[2] = 0x01;               /* ShareType = DISK */
  b[3] = 0;                  /* Reserved */
  put32(b + 4, 0);           /* ShareFlags */
  put32(b + 8, 0);           /* Capabilities */
  put32(b + 12, 0x001F01FF); /* MaximalAccess (full) */
  return send_msg(s, s->tx, SMB2_HDR + 16);
}

static int do_tree_disconnect(struct smb_conn *s, const uint8_t *req)
{
  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 4);
  put16(b + 2, 0);
  return send_msg(s, s->tx, SMB2_HDR + 4);
}

static int do_logoff(struct smb_conn *s, const uint8_t *req)
{
  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 4);
  put16(b + 2, 0);
  return send_msg(s, s->tx, SMB2_HDR + 4);
}

/* --------------------------------------------------------- handles + names */

#define FID_MAGIC 0x4F574F5346494421ull /* "OWOSFID!" in the FileId tail */

static int handle_alloc(struct smb_conn *s)
{
  for (int i = 0; i < SMB_MAX_HANDLES; i++)
    if (!s->handles[i].used) {
      struct smb_handle *h = &s->handles[i];
      h->used = true;
      h->is_dir = false;
      h->path[0] = 0;
      h->size = 0;
      h->enum_pos = 0;
      return i;
    }
  return -1;
}

static struct smb_handle *handle_get(struct smb_conn *s, const uint8_t *fid)
{
  uint64_t idx = load_le64(fid);
  if (idx >= SMB_MAX_HANDLES || !s->handles[idx].used)
    return NULL;
  return &s->handles[idx];
}

static void put_fileid(uint8_t *p, uint64_t idx)
{
  put64(p, idx);
  put64(p + 8, FID_MAGIC);
}

/* Convert an SMB share-relative path (UTF-16LE, ASCII assumed, backslash or
 * forward-slash separators) to an absolute owofs path "/a/b/c". An empty input
 * yields "/" (the share root). Separators are normalized and collapsed. */
static uint32_t smb_to_path(const uint8_t *src, uint32_t bytelen, char *dst, uint32_t cap)
{
  uint32_t n = 0;
  dst[n++] = '/';
  for (uint32_t i = 0; i + 1 < bytelen && n + 1 < cap; i += 2) {
    char ch = (char)src[i];
    if (ch == '\\' || ch == '/') {
      if (dst[n - 1] != '/')
        dst[n++] = '/'; /* collapse separators */
    } else {
      dst[n++] = ch;
    }
  }
  if (n > 1 && dst[n - 1] == '/')
    n--; /* drop any trailing slash */
  dst[n] = '\0';
  return n;
}

/* The virtual k-th entry of directory `dirpath`: 0=".", 1="..", then each
 * owofs child (files and subdirectories). */
static bool dir_entry_at(const char *dirpath, int idx, char *name, bool *is_dir, uint32_t *size, uint32_t *mtime)
{
  *mtime = 0;
  if (idx == 0) {
    name[0] = '.';
    name[1] = 0;
    *is_dir = true;
    *size = 0;
    return true;
  }
  if (idx == 1) {
    name[0] = '.';
    name[1] = '.';
    name[2] = 0;
    *is_dir = true;
    *size = 0;
    return true;
  }
  struct dirent_u de;
  int r = readdir(dirpath, idx - 2, &de);
  if (r != 1)
    return false;
  int i = 0;
  for (; de.name[i] && i < 63; i++)
    name[i] = de.name[i];
  name[i] = 0;
  *is_dir = de.is_dir;
  *size = de.size;
  /* readdir carries no mtime; stat the child to recover its owofs stamp. */
  char child[192];
  uint32_t n = 0;
  for (const char *q = dirpath; *q && n + 1 < sizeof child; q++)
    child[n++] = *q;
  if (n == 0 || child[n - 1] != '/')
    child[n++] = '/';
  for (int j = 0; name[j] && n + 1 < sizeof child; j++)
    child[n++] = name[j];
  child[n] = 0;
  struct statinfo cst;
  if (stat(child, &cst) == 0)
    *mtime = cst.mtime;
  return true;
}

/* ------------------------------------------------------------- CREATE */

static int do_create(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  const uint8_t *body = req + SMB2_HDR;
  uint16_t noff = load_le16(body + 44);
  uint16_t nlen = load_le16(body + 46);
  char path[128];
  if (nlen && (uint32_t)noff + nlen <= reqlen)
    smb_to_path(req + noff, nlen, path, sizeof path);
  else {
    path[0] = '/';
    path[1] = 0;
  } /* empty name -> share root */

  struct statinfo st;
  if (stat(path, &st) != 0)
    return do_error(s, req, STATUS_OBJECT_NAME_NOTFOUND);
  bool is_dir = st.is_dir;
  uint32_t size = st.size;

  int idx = handle_alloc(s);
  if (idx < 0)
    return do_error(s, req, STATUS_NOT_SUPPORTED);
  struct smb_handle *h = &s->handles[idx];
  h->is_dir = is_dir;
  h->size = is_dir ? 0 : size;
  h->mtime = st.mtime;
  h->enum_pos = 0;
  int i = 0;
  for (; i < (int)sizeof h->path - 1 && path[i]; i++)
    h->path[i] = path[i];
  h->path[i] = 0;

  uint64_t ft = owofs_filetime(h->mtime);
  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  for (int k = 0; k < 88; k++)
    b[k] = 0;
  put16(b + 0, 89);                    /* StructureSize */
  put32(b + 4, 1);                     /* CreateAction = FILE_OPENED */
  put64(b + 8, ft);                    /* CreationTime */
  put64(b + 16, ft);                   /* LastAccessTime */
  put64(b + 24, ft);                   /* LastWriteTime */
  put64(b + 32, ft);                   /* ChangeTime */
  put64(b + 40, size);                 /* AllocationSize */
  put64(b + 48, size);                 /* EndOfFile */
  put32(b + 56, is_dir ? 0x10 : 0x80); /* FileAttributes: DIRECTORY/NORMAL */
  put_fileid(b + 64, (uint64_t)idx);   /* FileId */
  return send_msg(s, s->tx, SMB2_HDR + 88);
}

/* ----------------------------------------------------- QUERY_DIRECTORY */

/* Build one directory-info entry; supports FileBothDirectoryInformation (3) and
 * FileIdBothDirectoryInformation (37). Returns bytes written (8-byte padded).
 */
static uint32_t build_dirent(uint8_t *p, int cls, const char *name, bool is_dir, uint32_t size, uint32_t mtime,
                             int is_last)
{
  uint64_t ft = owofs_filetime(mtime);
  uint32_t fixed = (cls == 37) ? 104 : 94;
  for (uint32_t i = 0; i < fixed; i++)
    p[i] = 0;
  put64(p + 8, ft);                    /* CreationTime */
  put64(p + 16, ft);                   /* LastAccessTime */
  put64(p + 24, ft);                   /* LastWriteTime */
  put64(p + 32, ft);                   /* ChangeTime */
  put64(p + 40, is_dir ? 0 : size);    /* EndOfFile */
  put64(p + 48, is_dir ? 0 : size);    /* AllocationSize */
  put32(p + 56, is_dir ? 0x10 : 0x80); /* FileAttributes */
  uint32_t namelen = put_utf16(p + fixed, name);
  put32(p + 60, namelen); /* FileNameLength */
  uint32_t total = fixed + namelen;
  uint32_t pad = (8 - (total & 7)) & 7;
  for (uint32_t i = 0; i < pad; i++)
    p[total + i] = 0;
  total += pad;
  put32(p + 0, is_last ? 0 : total); /* NextEntryOffset */
  return total;
}

static int do_query_directory(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  (void)reqlen;
  const uint8_t *body = req + SMB2_HDR;
  int cls = body[2];
  int flags = body[3];
  struct smb_handle *h = handle_get(s, body + 8);
  if (!h || !h->is_dir)
    return do_error(s, req, STATUS_NOT_SUPPORTED);
  if (cls != 3 && cls != 37)
    cls = 37;       /* default to FileIdBothDirInfo */
  if (flags & 0x01) /* SMB2_RESTART_SCANS */
    h->enum_pos = 0;
  int single = flags & 0x02; /* SMB2_RETURN_SINGLE_ENTRY */

  uint8_t *out = s->tx + SMB2_HDR + 8; /* buffer follows the 8-byte body */
  uint32_t total = 0, last_start = 0;
  int produced = 0;
  for (;;) {
    char nm[64];
    bool is_dir;
    uint32_t sz, mt; /* >= owofs FS_NAME_LEN */
    if (!dir_entry_at(h->path, h->enum_pos, nm, &is_dir, &sz, &mt))
      break;
    last_start = total;
    total += build_dirent(out + total, cls, nm, is_dir, sz, mt, 0);
    produced++;
    h->enum_pos++;
    if (single || total > 60000)
      break;
  }
  if (produced == 0)
    return do_error(s, req, STATUS_NO_MORE_FILES);
  put32(out + last_start, 0); /* terminate the chain */

  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 9);            /* StructureSize */
  put16(b + 2, SMB2_HDR + 8); /* OutputBufferOffset */
  put32(b + 4, total);        /* OutputBufferLength */
  return send_msg(s, s->tx, SMB2_HDR + 8 + total);
}

/* ------------------------------------------------------------- READ */

static int do_read(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  (void)reqlen;
  const uint8_t *body = req + SMB2_HDR;
  uint32_t len = load_le32(body + 4);
  uint64_t off = load_le64(body + 8);
  struct smb_handle *h = handle_get(s, body + 16);
  if (!h || h->is_dir)
    return do_error(s, req, STATUS_NOT_SUPPORTED);
  if (off >= h->size)
    return do_error(s, req, STATUS_END_OF_FILE);

  uint32_t avail = h->size - (uint32_t)off;
  uint32_t cap = SMB_MSG_MAX - (SMB2_HDR + 16);
  uint32_t n = len < avail ? len : avail;
  if (n > cap)
    n = cap;

  uint8_t *data = s->tx + SMB2_HDR + 16;
  int r = readfile_at(h->path, (uint32_t)off, data, n);
  if (r < 0)
    return do_error(s, req, STATUS_END_OF_FILE);
  n = (uint32_t)r;

  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 17);     /* StructureSize */
  b[2] = SMB2_HDR + 16; /* DataOffset */
  b[3] = 0;
  put32(b + 4, n);  /* DataLength */
  put32(b + 8, 0);  /* DataRemaining */
  put32(b + 12, 0); /* Reserved2 */
  return send_msg(s, s->tx, SMB2_HDR + 16 + n);
}

/* ------------------------------------------------------------- CLOSE */

static int do_close(struct smb_conn *s, const uint8_t *req)
{
  const uint8_t *body = req + SMB2_HDR;
  struct smb_handle *h = handle_get(s, body + 8);
  uint32_t size = 0, attrs = 0x80, mtime = 0;
  if (h) {
    size = h->is_dir ? 0 : h->size;
    attrs = h->is_dir ? 0x10 : 0x80;
    mtime = h->mtime;
    h->used = false;
  }
  uint64_t ft = owofs_filetime(mtime);
  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  for (int k = 0; k < 60; k++)
    b[k] = 0;
  put16(b + 0, 60);     /* StructureSize */
  put64(b + 8, ft);     /* CreationTime */
  put64(b + 16, ft);    /* LastAccessTime */
  put64(b + 24, ft);    /* LastWriteTime */
  put64(b + 32, ft);    /* ChangeTime */
  put64(b + 40, size);  /* AllocationSize */
  put64(b + 48, size);  /* EndOfFile */
  put32(b + 56, attrs); /* FileAttributes */
  return send_msg(s, s->tx, SMB2_HDR + 60);
}

/* ------------------------------------------------------------- QUERY_INFO */

static uint32_t put_basic_info(uint8_t *p, bool is_dir, uint64_t ft)
{
  put64(p + 0, ft);                    /* CreationTime */
  put64(p + 8, ft);                    /* LastAccessTime */
  put64(p + 16, ft);                   /* LastWriteTime */
  put64(p + 24, ft);                   /* ChangeTime */
  put32(p + 32, is_dir ? 0x10 : 0x80); /* FileAttributes */
  put32(p + 36, 0);                    /* Reserved */
  return 40;
}

static uint32_t put_standard_info(uint8_t *p, bool is_dir, uint32_t size)
{
  put64(p + 0, size);                /* AllocationSize */
  put64(p + 8, size);                /* EndOfFile */
  put32(p + 16, 1);                  /* NumberOfLinks */
  p[20] = 0;                         /* DeletePending */
  p[21] = (uint8_t)(is_dir ? 1 : 0); /* Directory */
  put16(p + 22, 0);                  /* Reserved */
  return 24;
}

static int do_query_info(struct smb_conn *s, const uint8_t *req, uint32_t reqlen)
{
  (void)reqlen;
  const uint8_t *body = req + SMB2_HDR;
  int itype = body[2];
  int cls = body[3];
  uint8_t *out = s->tx + SMB2_HDR + 8;
  uint32_t olen = 0;
  uint32_t status = STATUS_SUCCESS;

  if (itype == 0x01) { /* FILE info */
    struct smb_handle *h = handle_get(s, body + 24);
    bool is_dir = h ? h->is_dir : true;
    uint32_t size = h ? h->size : 0;
    const char *nm = h ? h->path : "";
    uint64_t ft = owofs_filetime(h ? h->mtime : 0);
    switch (cls) {
    case 4:
      olen = put_basic_info(out, is_dir, ft);
      break;
    case 5:
      olen = put_standard_info(out, is_dir, size);
      break;
    case 6:
      put64(out, 0);
      olen = 8;
      break; /* FileInternalInformation */
    case 34: /* FileNetworkOpenInformation */
      put64(out, ft);
      put64(out + 8, ft);
      put64(out + 16, ft);
      put64(out + 24, ft);
      put64(out + 32, size);
      put64(out + 40, size);
      put32(out + 48, is_dir ? 0x10 : 0x80);
      put32(out + 52, 0);
      olen = 56;
      break;
    case 18: { /* FileAllInformation */
      uint32_t o = 0;
      o += put_basic_info(out + o, is_dir, ft);
      o += put_standard_info(out + o, is_dir, size);
      put64(out + o, 0);
      o += 8; /* InternalInformation */
      put32(out + o, 0);
      o += 4; /* EaInformation */
      put32(out + o, 0x001F01FF);
      o += 4; /* AccessInformation */
      put64(out + o, 0);
      o += 8; /* PositionInformation */
      put32(out + o, 0);
      o += 4; /* ModeInformation */
      put32(out + o, 0);
      o += 4; /* AlignmentInformation */
      uint32_t nl = put_utf16(out + o + 4, nm);
      put32(out + o, nl);
      o += 4 + nl; /* NameInformation */
      olen = o;
      break;
    }
    default:
      status = STATUS_NOT_SUPPORTED;
      break;
    }
  } else if (itype == 0x02) { /* FILESYSTEM info */
    /* Real owofs capacity/free, reported in 512-byte allocation units. */
    struct statfs_info fs;
    uint64_t total_u = 32768, avail_u = 32764; /* fallback if statfs fails */
    if (statfs("/", &fs) == 0) {
      total_u = fs.total_bytes / 512;
      avail_u = fs.free_bytes / 512;
    }
    uint32_t total = (uint32_t)total_u;
    uint32_t avail = (uint32_t)avail_u;
    switch (cls) {
    case 1: /* FileFsVolumeInformation */
      put64(out, FAKE_FILETIME);
      put32(out + 8, 0x4F574F53);
      out[16] = 0;
      out[17] = 0;
      {
        uint32_t nl = put_utf16(out + 18, "OWOS");
        put32(out + 12, nl);
        olen = 18 + nl;
      }
      break;
    case 3: /* FileFsSizeInformation */
      put64(out, total);
      put64(out + 8, avail);
      put32(out + 16, 1);
      put32(out + 20, 512);
      olen = 24;
      break;
    case 4: /* FileFsDeviceInformation */
      put32(out, 0x7);
      put32(out + 4, 0);
      olen = 8;
      break;
    case 5: /* FileFsAttributeInformation */
      put32(out, 0x00000003);
      put32(out + 4, 255);
      {
        uint32_t nl = put_utf16(out + 12, "OWOFS");
        put32(out + 8, nl);
        olen = 12 + nl;
      }
      break;
    case 7: /* FileFsFullSizeInformation */
      put64(out, total);
      put64(out + 8, avail);
      put64(out + 16, avail);
      put32(out + 24, 1);
      put32(out + 28, 512);
      olen = 32;
      break;
    default:
      status = STATUS_NOT_SUPPORTED;
      break;
    }
  } else {
    status = STATUS_NOT_SUPPORTED;
  }

  if (status != STATUS_SUCCESS)
    return do_error(s, req, status);
  build_header(s, req, STATUS_SUCCESS);
  uint8_t *b = s->tx + SMB2_HDR;
  put16(b + 0, 9);            /* StructureSize */
  put16(b + 2, SMB2_HDR + 8); /* OutputBufferOffset */
  put32(b + 4, olen);         /* OutputBufferLength */
  return send_msg(s, s->tx, SMB2_HDR + 8 + olen);
}

/* ------------------------------------------------------------- dispatch */

static struct smb_conn conn; /* one connection at a time */

static void serve(int fd)
{
  struct smb_conn *s = &conn;
  s->fd = fd;
  s->session_id = 0;
  s->closed = false;
  for (int i = 0; i < SMB_MAX_HANDLES; i++)
    s->handles[i].used = false;

  for (;;) {
    int n = read_msg(s);
    if (n < 0)
      return;
    const uint8_t *req = s->rx;
    if (req[0] != SMB2_MAGIC0 || req[1] != 'S' || req[2] != 'M' || req[3] != 'B')
      return; /* not SMB2 (SMB1 negotiate etc.) — drop */

    uint16_t cmd = load_le16(req + 12);
    int rc;
    switch (cmd) {
    case SMB2_NEGOTIATE:
      rc = do_negotiate(s, req, (uint32_t)n);
      break;
    case SMB2_SESSION_SETUP:
      rc = do_session_setup(s, req, (uint32_t)n);
      break;
    case SMB2_TREE_CONNECT:
      rc = do_tree_connect(s, req, (uint32_t)n);
      break;
    case SMB2_TREE_DISCONNECT:
      rc = do_tree_disconnect(s, req);
      break;
    case SMB2_LOGOFF:
      rc = do_logoff(s, req);
      break;
    case SMB2_CREATE:
      rc = do_create(s, req, (uint32_t)n);
      break;
    case SMB2_QUERY_DIRECTORY:
      rc = do_query_directory(s, req, (uint32_t)n);
      break;
    case SMB2_READ:
      rc = do_read(s, req, (uint32_t)n);
      break;
    case SMB2_CLOSE:
      rc = do_close(s, req);
      break;
    case SMB2_QUERY_INFO:
      rc = do_query_info(s, req, (uint32_t)n);
      break;
    default:
      rc = do_error(s, req, STATUS_NOT_IMPLEMENTED);
      break;
    }
    if (rc < 0)
      return;
  }
}

int main(void)
{
  int lfd = net_listen(445);
  if (lfd < 0) {
    puts("smbd: cannot listen on port 445\n");
    return 1;
  }
  for (;;) {
    int c = net_accept(lfd);
    if (c < 0) {
      if (c == NET_EINTR)
        break;
      continue;
    }
    serve(c);
    net_close(c);
  }
  net_close(lfd);
  return 0;
}

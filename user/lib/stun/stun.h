/* stun.h — STUN (RFC 5389 / 8489) message codec + a reflexive-address query.
 *
 * STUN is the NAT-traversal signaling protocol WebRTC/ICE builds on. This is the
 * generic substrate, named for what it is (not its first consumer): it encodes
 * and decodes Binding messages with the attributes an ICE agent needs
 * (XOR-MAPPED-ADDRESS, USERNAME, MESSAGE-INTEGRITY, FINGERPRINT, PRIORITY,
 * USE-CANDIDATE, ICE-CONTROLLING/CONTROLLED), and offers a one-call
 * server-reflexive lookup over a UDP socket for candidate gathering.
 *
 * Links with $(CRYPTO) (hmac_sha1 for MESSAGE-INTEGRITY) and, for stun_query's
 * name->IP, $(RESOLVE). IPv4 only (the kernel transport is IPv4). */
#ifndef STUN_H
#define STUN_H

#define STUN_MAGIC   0x2112A442u   /* the fixed magic cookie (bytes 4..7)        */

/* Message types (class + method). Binding is the only method STUN defines. */
#define STUN_BINDING_REQUEST     0x0001
#define STUN_BINDING_INDICATION  0x0011
#define STUN_BINDING_SUCCESS     0x0101
#define STUN_BINDING_ERROR       0x0111

/* Attribute types. 0x0000-0x7FFF are comprehension-required, 0x8000+ optional. */
#define STUN_ATTR_MAPPED_ADDRESS      0x0001
#define STUN_ATTR_USERNAME            0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY   0x0008
#define STUN_ATTR_ERROR_CODE          0x0009
#define STUN_ATTR_UNKNOWN_ATTRIBUTES  0x000A
#define STUN_ATTR_XOR_MAPPED_ADDRESS  0x0020
#define STUN_ATTR_PRIORITY            0x0024   /* ICE (RFC 8445)                  */
#define STUN_ATTR_USE_CANDIDATE       0x0025   /* ICE: zero-length flag           */
#define STUN_ATTR_SOFTWARE            0x8022
#define STUN_ATTR_ALTERNATE_SERVER    0x8023
#define STUN_ATTR_FINGERPRINT         0x8028
#define STUN_ATTR_ICE_CONTROLLED      0x8029   /* ICE: 8-byte tie-breaker         */
#define STUN_ATTR_ICE_CONTROLLING     0x802A

#define STUN_MAX  512      /* a Binding message never approaches this            */

/* An IPv4 transport address (a mapped address or an ICE candidate endpoint). */
typedef struct {
	unsigned char ip[4];
	unsigned      port;
} stun_addr;

/* A message being built or the raw bytes just received. `len` is the total on
 * the wire including the 20-byte header; the header length field is kept in sync
 * by every stun_add_*. */
typedef struct {
	unsigned char buf[STUN_MAX];
	int           len;
} stun_msg;

/* ---- building ---------------------------------------------------------- */

/* Fill a fresh 12-byte transaction id from the kernel CSPRNG (getrandom). */
void stun_txid(unsigned char txid[12]);

/* Start a message: header only (length 0), the given type + transaction id. */
void stun_init(stun_msg *m, unsigned type, const unsigned char txid[12]);

/* Append attributes. Each pads the value to a 4-byte boundary and refreshes the
 * header length. Return 1 on success, 0 if the message would overflow STUN_MAX. */
int stun_add_raw(stun_msg *m, unsigned attr, const void *val, int vlen);
int stun_add_u32(stun_msg *m, unsigned attr, unsigned v);
int stun_add_u64(stun_msg *m, unsigned attr, unsigned hi, unsigned lo);
int stun_add_flag(stun_msg *m, unsigned attr);            /* zero-length         */
int stun_add_string(stun_msg *m, unsigned attr, const char *s);
int stun_add_xor_mapped(stun_msg *m, unsigned attr, const stun_addr *a);

/* MESSAGE-INTEGRITY: HMAC-SHA1 keyed by (key,keylen) over the message so far,
 * with the header length pre-adjusted to include this attribute (per RFC 5389
 * §15.4). Add it last, before FINGERPRINT only. */
int stun_add_message_integrity(stun_msg *m, const void *key, int keylen);

/* FINGERPRINT: CRC-32 of the message so far (length pre-adjusted) XOR 0x5354554E.
 * The very last attribute. */
int stun_add_fingerprint(stun_msg *m);

/* ---- parsing ----------------------------------------------------------- */

/* Structurally valid STUN? (length >= 20, top two type bits clear, magic cookie,
 * declared attribute length inside the buffer and 4-aligned.) 1 / 0. */
int stun_valid(const unsigned char *buf, int len);

/* The 14-bit message type (class + method) from a validated header. */
unsigned stun_type(const unsigned char *buf);

/* Does buf's transaction id equal txid[12]? (Match a reply to its request.) */
int stun_txid_eq(const unsigned char *buf, const unsigned char txid[12]);

/* Locate attribute `attr`; returns its value pointer and length via *vlen, or
 * NULL if absent. The first occurrence wins (RFC: duplicates ignore the rest). */
const unsigned char *stun_find(const unsigned char *buf, int len,
                               unsigned attr, int *vlen);

/* Decode the (XOR-)MAPPED-ADDRESS into out (XOR undone against cookie+txid).
 * Prefers XOR-MAPPED-ADDRESS, falls back to MAPPED-ADDRESS. IPv4 only. 1 / 0. */
int stun_get_mapped(const unsigned char *buf, int len, stun_addr *out);

/* Verify MESSAGE-INTEGRITY against (key,keylen). Patches the length field in
 * place for the HMAC computation and restores it, so buf must be writable. 1 if
 * the tag matches, 0 otherwise (absent, wrong length, or mismatch). */
int stun_check_integrity(unsigned char *buf, int len, const void *key, int keylen);

/* ---- convenience ------------------------------------------------------- */

#define STUN_EINTR (-4)   /* a cooperative Ctrl+C arrived mid-call               */

/* Discover this host's server-reflexive transport address (the public IPv4:port
 * a NAT maps our UDP socket to) via a Binding request to host:port. host may be
 * a name or a dotted-quad; port is usually 3478. Retransmits a few times.
 * Returns 1 (out filled), 0 (no response / parse failure), or STUN_EINTR. */
int stun_query(const char *host, int port, stun_addr *out);

#endif /* STUN_H */

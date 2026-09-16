/* stun_test.c — host KAT for user/lib/stun/stun.c against RFC 5769 test vectors.
 *
 * Verifies the padding-independent codec paths against the RFC's exact bytes:
 * XOR-MAPPED-ADDRESS decode, MESSAGE-INTEGRITY (HMAC-SHA1) verify, and a
 * build->parse round-trip. RFC 5769 pads USERNAME with spaces while a
 * spec-compliant sender (and stun.c) zero-pads, so a whole-message compare of a
 * *built* request is only exact up to the first odd-length attribute's padding;
 * that structural prefix is checked explicitly.  Build:
 *   cc -std=c11 -Iscripts -Iuser/lib/stun -Iuser/lib/crypto -Iuser/lib/resolve \
 *      -Iuser/lib/zlib tests/stun_test.c user/lib/stun/stun.c \
 *      user/lib/crypto/crypto.c user/lib/zlib/crc32.c \
 *      -o /tmp/stun_test && /tmp/stun_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include <string.h>
#include "owos.h"
#include "stun.h"

/* Parse a hex string (spaces ignored) into out; returns the byte count. */
static int hex(const char *s, unsigned char *out, int max)
{
	int n = 0, hi = -1;
	for (; *s && n < max; s++) {
		int d;
		if (*s == ' ' || *s == '\n' || *s == '\t') continue;
		d = (*s <= '9') ? *s - '0' : (*s | 32) - 'a' + 10;
		if (hi < 0) hi = d;
		else { out[n++] = (unsigned char)((hi << 4) | d); hi = -1; }
	}
	return n;
}

/* RFC 5769 short-term credential: the HMAC-SHA1 key is the password itself. */
static const char *PASS = "VOkJxbRl1RmTxUk/WvJxBt";

/* RFC 5769 §2.2 Sample IPv4 Response — mapped address 192.0.2.1:32853. */
static const char *RESP_HEX =
	"0101 003c 2112a442 b7e7a701 bc34d686 fa87dfae"
	"8022 000b 74657374 20766563 746f7220"
	"0020 0008 0001a147 e112a643"
	"0008 0014 2b91f599 fd9e90c3 8c7489f9 2af9ba53 f06be7d7"
	"8028 0004 c07d4c96";

int main(void)
{
	unsigned char req[STUN_MAX], resp[STUN_MAX];
	int rn, pn;

	pn = hex(RESP_HEX, resp, sizeof resp);
	expect(pn == 80);

	/* --- §2.2 XOR-MAPPED-ADDRESS decode --- */
	test("RFC 5769 response decode");
	{
		stun_addr a;
		int got = stun_get_mapped(resp, pn, &a);
		expect(stun_valid(resp, pn));
		expect(stun_type(resp) == STUN_BINDING_SUCCESS);
		expect(got &&
		   a.ip[0] == 192 && a.ip[1] == 0 && a.ip[2] == 2 && a.ip[3] == 1 &&
		   a.port == 32853);
	}

	/* --- §2.2 MESSAGE-INTEGRITY (HMAC-SHA1) verify against exact bytes --- */
	test("MESSAGE-INTEGRITY verification");
	expect(
	   stun_check_integrity(resp, pn, PASS, (int)strlen(PASS)) == 1);
	{
		unsigned char bad[STUN_MAX];
		memcpy(bad, resp, (size_t)pn);
		bad[24] ^= 0x01;   /* flip a byte inside SOFTWARE (covered by the HMAC) */
		expect(
		   stun_check_integrity(bad, pn, PASS, (int)strlen(PASS)) == 0);
	}

	/* --- §2.1 request: rebuild the exact bytes and verify integrity --- */
	test("RFC 5769 request verification");
	{
		/* Assemble the RFC request byte-for-byte (0x20 USERNAME padding). */
		static const unsigned char REQ[] = {
			0x00,0x01,0x00,0x58, 0x21,0x12,0xa4,0x42,
			0xb7,0xe7,0xa7,0x01, 0xbc,0x34,0xd6,0x86, 0xfa,0x87,0xdf,0xae,
			0x80,0x22,0x00,0x10, 'S','T','U','N',' ','t','e','s',
			                     't',' ','c','l','i','e','n','t',
			0x00,0x24,0x00,0x04, 0x6e,0x00,0x01,0xff,
			0x80,0x29,0x00,0x08, 0x93,0x2f,0xf9,0xb1, 0x51,0x26,0x3b,0x36,
			0x00,0x06,0x00,0x09, 'e','v','t','j',':','h','6','v','Y',0x20,0x20,0x20,
			0x00,0x08,0x00,0x14, 0x9a,0xea,0xa7,0x0c, 0xbf,0xd8,0xcb,0x56,
			                     0x78,0x1e,0xf2,0xb5, 0xb2,0xd3,0xf2,0x49, 0xc1,0xb5,0x71,0xa2,
			0x80,0x28,0x00,0x04, 0xe5,0x7a,0x3b,0xcf
		};
		memcpy(req, REQ, sizeof REQ);
		rn = (int)sizeof REQ;
		expect(stun_valid(req, rn));
		expect(stun_type(req) == STUN_BINDING_REQUEST);
		expect(
		   stun_check_integrity(req, rn, PASS, (int)strlen(PASS)) == 1);
	}

	/* --- build->parse round-trip through the encoder --- */
	test("encode and parse round-trip");
	{
		unsigned char txid[12] = {0xb7,0xe7,0xa7,0x01,0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae};
		stun_addr me = { {203,0,113,7}, 51234 };
		stun_msg m;
		stun_addr back;

		stun_init(&m, STUN_BINDING_SUCCESS, txid);
		stun_add_string(&m, STUN_ATTR_SOFTWARE, "OwOS");
		stun_add_u32(&m, STUN_ATTR_PRIORITY, 0x6e0001ffu);
		stun_add_u64(&m, STUN_ATTR_ICE_CONTROLLING, 0x11223344u, 0x55667788u);
		stun_add_xor_mapped(&m, STUN_ATTR_XOR_MAPPED_ADDRESS, &me);
		stun_add_message_integrity(&m, PASS, (int)strlen(PASS));
		stun_add_fingerprint(&m);

		expect(stun_valid(m.buf, m.len));
		expect(stun_txid_eq(m.buf, txid));
		expect(
		   stun_get_mapped(m.buf, m.len, &back) &&
		   memcmp(back.ip, me.ip, 4) == 0 && back.port == me.port);
		expect(
		   stun_check_integrity(m.buf, m.len, PASS, (int)strlen(PASS)) == 1);
	}

	/* --- structural prefix of a built §2.1 request matches the RFC exactly up
	 *     to the first zero-vs-space padding divergence (USERNAME padding). --- */
	test("RFC 5769 request encoding");
	{
		unsigned char txid[12] = {0xb7,0xe7,0xa7,0x01,0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae};
		static const unsigned char REQ_PREFIX[] = {
			0x00,0x01,0x00,0x58, 0x21,0x12,0xa4,0x42,
			0xb7,0xe7,0xa7,0x01, 0xbc,0x34,0xd6,0x86, 0xfa,0x87,0xdf,0xae,
			0x80,0x22,0x00,0x10, 'S','T','U','N',' ','t','e','s',
			                     't',' ','c','l','i','e','n','t',
			0x00,0x24,0x00,0x04, 0x6e,0x00,0x01,0xff,
			0x80,0x29,0x00,0x08, 0x93,0x2f,0xf9,0xb1, 0x51,0x26,0x3b,0x36,
			0x00,0x06,0x00,0x09, 'e','v','t','j',':','h','6','v','Y'
		};
		stun_msg m;
		stun_init(&m, STUN_BINDING_REQUEST, txid);
		stun_add_string(&m, STUN_ATTR_SOFTWARE, "STUN test client");
		stun_add_u32(&m, STUN_ATTR_PRIORITY, 0x6e0001ffu);
		stun_add_u64(&m, STUN_ATTR_ICE_CONTROLLED, 0x932ff9b1u, 0x51263b36u);
		stun_add_string(&m, STUN_ATTR_USERNAME, "evtj:h6vY");
		/* header length field must already read 0x58 as MI+FINGERPRINT get added */
		stun_add_message_integrity(&m, PASS, (int)strlen(PASS));
		stun_add_fingerprint(&m);
		expect(
		   (m.buf[2] << 8 | m.buf[3]) == 0x58);
		expect(
		   m.len >= (int)sizeof REQ_PREFIX &&
		   memcmp(m.buf, REQ_PREFIX, sizeof REQ_PREFIX) == 0);
	}

	return 0;
}

/* Host stubs: the codec never touches the wire, but stun_query references these. */
int getrandom(void *buf, int len) { memset(buf, 0x5a, (size_t)len); return len; }
int net_dial(const char *h, int p, int proto) { (void)h; (void)p; (void)proto; return -1; }
int net_send(int h, const void *b, int l) { (void)h; (void)b; (void)l; return -1; }
int net_recv(int h, void *b, int m) { (void)h; (void)b; (void)m; return 0; }
int net_close(int h) { (void)h; return 0; }

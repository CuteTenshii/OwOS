/* uuid.h: RFC 9562 UUIDs. Parse and format, compare, and generate version 4 (random), 5 (SHA-1 of a namespace and
 * name) and 7 (Unix-millisecond time plus random, so values sort by creation). Link $(UUID) $(CRYPTO). The generators live in gen.c, so a program that supplies its own randomness
 * can link uuid.o alone. */
#ifndef OWOS_UUID_H
#define OWOS_UUID_H

typedef unsigned char uuid_t[16];

#define UUID_STR_LEN 37 /* "8-4-4-4-12" plus the NUL */

/* The RFC 9562 namespaces for uuid_v5 */
extern const uuid_t UUID_NS_DNS, UUID_NS_URL, UUID_NS_OID, UUID_NS_X500;

void uuid_generate_v4(uuid_t out);
void uuid_generate_v7(uuid_t out);

/* Deterministic variants for callers that bring their own randomness or clock */
void uuid_v4_from(uuid_t out, const unsigned char rnd[16]);
void uuid_v7_from(uuid_t out, unsigned long long unix_ms, const unsigned char rnd[10]);
void uuid_v5(uuid_t out, const uuid_t ns, const void *name, int len);

/* Lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into out[UUID_STR_LEN] (uppercase when `upper`) */
void uuid_to_string(const uuid_t u, char *out, int upper);

/* Accepts the dashed form, 32 bare hex digits, "{...}" and "urn:uuid:..." in either case. 0 on success, -1 if
 * malformed (out is then untouched). */
int uuid_parse(const char *s, uuid_t out);

int uuid_compare(const uuid_t a, const uuid_t b); /* <0, 0, >0 byte-wise, which orders v7 by time */
int uuid_is_nil(const uuid_t u);
int uuid_version(const uuid_t u); /* the version nibble, 1..8 */
int uuid_variant(const uuid_t u); /* 2 for RFC 9562 (10xx) */
unsigned long long uuid_v7_time(const uuid_t u); /* the embedded Unix milliseconds */

#endif

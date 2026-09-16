/* base64.h — standard base64 (RFC 4648: '+'/'/' alphabet, '=' padding).
 *
 * Encoding and decoding are allocation-free — the caller sizes the destination
 * with the *_len / *_cap helpers. Pure computation, no syscalls; the one
 * canonical base64 for the browser's data:/atob, the CLI, and any other
 * consumer. */
#ifndef OWOS_BASE64_H
#define OWOS_BASE64_H

/* Bytes an encode of `n` input bytes produces, excluding the NUL terminator. */
int base64_encoded_len(int n);
/* Encode `n` bytes of `src` into `dst` (NUL-terminated). Returns the length
 * written excluding the NUL (== base64_encoded_len(n)). */
int base64_encode(const unsigned char *src, int n, char *dst);

/* An upper bound on the bytes a decode of `len` base64 chars can produce. */
int base64_decoded_cap(int len);
/* Decode `len` chars of `src` into `dst`, skipping ASCII whitespace and
 * stopping at '=' padding. Returns bytes written, or -1 on an invalid
 * character. */
int base64_decode(const char *src, int len, unsigned char *dst);

#endif /* OWOS_BASE64_H */

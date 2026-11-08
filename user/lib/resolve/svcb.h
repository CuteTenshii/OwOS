/* svcb.h — DNS response parsing for HTTPS (type 65) service bindings, RFC 9460. */
#ifndef SVCB_H
#define SVCB_H

#include <stdint.h>

#define DNS_TYPE_HTTPS 65

/* In-stream length of a name at msg[pos]: labels until a 0 byte (+1) or a compression pointer (+2).
 * 0 on malformed. */
int dns_name_len(const uint8_t *msg, int pos, int end);

/* Scan the answer section of the DNS response msg[0..len) for HTTPS ServiceMode records carrying an
 * `ech` SvcParam (key 5). Copies the ECHConfigList of the lowest-priority such record into out and
 * returns its length, or 0 if there is none or it does not fit in `max`. AliasMode records are ignored. */
int svcb_find_ech(const uint8_t *msg, int len, uint8_t *out, int max);

#endif /* SVCB_H */

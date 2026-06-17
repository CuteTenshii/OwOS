/* fmt.h — number <-> string conversion for OwOS user space.
 *
 * The reason this is its own library and not three lines of sprintf: printing a
 * double well means printing the *shortest* decimal string that parses back to
 * the identical bit pattern, and there is no libc here to lean on. dtoa() is a
 * from-scratch Dragon4 (Steele & White free-format, bignum); strtod() is the
 * inverse. The decimal/exponential layout dtoa() picks is the same shortest-
 * round-trip convention used by JS Number::toString, Python repr, Java, etc.
 * Nothing here allocates — callers pass fixed buffers.
 */
#ifndef OWOS_FMT_H
#define OWOS_FMT_H

/* Largest dtoa() output: "-1.7976931348623157e+308" plus NUL = 25 bytes. Round
 * up so callers can size with one named constant. */
#define DTOA_MAX 32

/* Format `v` as the shortest decimal string that round-trips to it: "NaN",
 * "Infinity"/"-Infinity", "0" (also for -0), then decimal or exponential by the
 * usual magnitude thresholds. Writes a NUL-terminated string into `out`
 * (>= DTOA_MAX bytes) and returns its length. */
int dtoa(double v, char *out);

/* Same, in `radix` (2..36, radix != 10): integer part exact, fractional part to
 * a capped number of digits. `out` must hold DTOA_RADIX_MAX bytes. */
#define DTOA_RADIX_MAX 1216
int dtoa_radix(double v, int radix, char *out);

/* Parse a decimal floating literal at `s` (optional sign, digits, '.', e/E
 * exponent, or the words Infinity/NaN). Correctly rounded to ~1 ulp. If `end`
 * is non-NULL it receives the first unconsumed character (standard C strtod
 * signature). Returns NaN with *end == s when nothing parsed. Leading
 * whitespace is NOT skipped. */
double strtod(const char *s, char **end);

/* Signed/unsigned integer to ASCII in radix 2..36 (lowercase digits). Writes a
 * NUL-terminated string and returns its length. `out` must hold 66 bytes
 * (64 binary digits + sign + NUL). */
int i64toa(long long v, char *out, int radix);
int u64toa(unsigned long long v, char *out, int radix);

#endif /* OWOS_FMT_H */

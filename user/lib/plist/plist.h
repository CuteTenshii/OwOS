/* plist.h: Apple property lists, XML and binary ("bplist00"), as a standalone value tree.
 *
 * Consumer-agnostic like user/lib/json: nodes are plain heap objects, not GC-managed, and the only dependencies are
 * libc, user/lib/base64 (<data>) and user/lib/math (strtod/dtoa for reals). Link $(PLIST) $(BASE64) $(MATH).
 *
 * Ownership: plist_parse returns a tree the caller releases with plist_free. The builders (plist_array_append,
 * plist_dict_set) always take ownership of the child: on failure they free it and return -1. plist_to_xml and
 * plist_to_bin return malloc'd buffers the caller free()s.
 *
 * Mapping: strings are UTF-8 and NUL-terminated, <data> is raw bytes, a date is seconds since 2001-01-01 UTC
 * (Core Foundation's epoch), UID is the keyed-archive reference type of the binary format, integers are signed
 * 64-bit (a 128-bit binary integer keeps its low 64 bits). A binary "null" or "fill" object is an error. */
#ifndef OWOS_PLIST_H
#define OWOS_PLIST_H

typedef enum {
  PLIST_BOOL,
  PLIST_INT,
  PLIST_REAL,
  PLIST_DATE,
  PLIST_STRING,
  PLIST_DATA,
  PLIST_UID,
  PLIST_ARRAY,
  PLIST_DICT
} plist_type;

typedef struct plist plist;
struct plist {
  plist_type type;
  union {
    int boolean;        /* PLIST_BOOL */
    long long integer;  /* PLIST_INT, PLIST_UID */
    double real;        /* PLIST_REAL, PLIST_DATE */
    struct {
      char *data;
      int len;
    } str; /* PLIST_STRING (NUL-terminated), PLIST_DATA */
    struct {
      plist **items;
      int count, cap;
    } array; /* PLIST_ARRAY */
    struct {
      char **keys;
      plist **vals;
      int count, cap;
    } dict; /* PLIST_DICT, in document order */
  } u;
};

typedef struct {
  int pos; /* byte offset of the problem */
  char msg[64];
} plist_error;

/* Parse a binary or XML plist (told apart by the "bplist00" magic). NULL with *err filled on bad input. */
plist *plist_parse(const void *buf, int len, plist_error *err);

char *plist_to_xml(const plist *v, int *out_len);
unsigned char *plist_to_bin(const plist *v, int *out_len);

void plist_free(plist *v);

plist *plist_new_bool(int b);
plist *plist_new_int(long long n);
plist *plist_new_real(double d);
plist *plist_new_date(double seconds_since_2001);
plist *plist_new_string(const char *s);
plist *plist_new_data(const void *bytes, int len);
plist *plist_new_uid(long long n);
plist *plist_new_array(void);
plist *plist_new_dict(void);

int plist_array_append(plist *arr, plist *item);
int plist_dict_set(plist *dict, const char *key, plist *val); /* replaces an existing key */

int plist_count(const plist *v);                      /* array/dict size, else 0 */
plist *plist_array_get(const plist *arr, int i);      /* NULL out of range */
plist *plist_dict_get(const plist *dict, const char *key); /* NULL if absent */

/* Typed reads that return a default when the node is missing or of another type. */
const char *plist_get_string(const plist *v);                  /* NULL unless a string */
int plist_get_number(const plist *v, double *out);             /* INT/REAL/DATE/BOOL as a double; 1 if numeric */
int plist_get_bool(const plist *v, int dflt);

#endif

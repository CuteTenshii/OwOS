/* bencode.c — recursive-descent bencode parser. See bencode.h. */

#include "bencode.h"

#define BEN_MAX_DEPTH                                                                                                  \
  32 /* cap nesting so hostile input can't blow the                                                                    \
      * kernel stack via parse_value recursion      */

/* Claim the next free node from the pool, returning its index or -1 when the
 * pool is exhausted. */
static int node_alloc(struct bencode *b)
{
  if (b->count >= b->cap)
    return -1;
  int idx = b->count++;
  struct ben_node *n = &b->nodes[idx];
  n->type = BEN_INT;
  n->first_child = n->next_sibling = -1;
  n->n_children = 0;
  n->ival = 0;
  n->str = n->raw = (const uint8_t *)0;
  n->slen = n->rawlen = 0;
  return idx;
}

/* Parse one value starting at *pos. On success advances *pos past it and
 * returns the node index; on any malformation returns -1. `depth` guards
 * against unbounded nesting. */
static int parse_value(struct bencode *b, uint32_t *pos, int depth)
{
  if (depth > BEN_MAX_DEPTH || *pos >= b->srclen)
    return -1;

  uint32_t start = *pos;
  int idx = node_alloc(b);
  if (idx < 0)
    return -1;

  uint8_t c = b->src[*pos];

  if (c == 'i') { /* i<digits>e             */
    (*pos)++;
    int neg = 0;
    if (*pos < b->srclen && b->src[*pos] == '-') {
      neg = 1;
      (*pos)++;
    }
    if (*pos >= b->srclen || b->src[*pos] < '0' || b->src[*pos] > '9')
      return -1; /* need at least one digit */
    uint64_t v = 0;
    int nd = 0;
    while (*pos < b->srclen && b->src[*pos] >= '0' && b->src[*pos] <= '9') {
      if (++nd > 19)
        return -1;                                 /* longer than any int64: reject */
      v = v * 10 + (uint64_t)(b->src[*pos] - '0'); /* ≤19 digits < 10^19 < 2^64: no overflow */
      (*pos)++;
    }
    if (*pos >= b->srclen || b->src[*pos] != 'e')
      return -1;
    (*pos)++;
    /* A 19-digit value can still exceed the int64 range (e.g.
     * i9999999999999999999e); reject rather than invoke signed-overflow UB in
     * the final store. */
    if (neg) {
      if (v > (uint64_t)INT64_MAX + 1)
        return -1;
      b->nodes[idx].ival = (v == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)v;
    } else {
      if (v > (uint64_t)INT64_MAX)
        return -1;
      b->nodes[idx].ival = (int64_t)v;
    }
    b->nodes[idx].type = BEN_INT;

  } else if (c >= '0' && c <= '9') { /* <len>:<bytes>          */
    uint32_t len = 0;
    while (*pos < b->srclen && b->src[*pos] >= '0' && b->src[*pos] <= '9') {
      len = len * 10 + (uint32_t)(b->src[*pos] - '0');
      if (len > b->srclen) /* clamp: can't exceed input */
        return -1;
      (*pos)++;
    }
    if (*pos >= b->srclen || b->src[*pos] != ':')
      return -1;
    (*pos)++;
    if (len > b->srclen - *pos) /* not enough bytes left   */
      return -1;
    b->nodes[idx].type = BEN_STR;
    b->nodes[idx].str = b->src + *pos;
    b->nodes[idx].slen = len;
    *pos += len;

  } else if (c == 'l' || c == 'd') { /* list / dict            */
    int is_dict = (c == 'd');
    (*pos)++;
    int prev = -1, pairs = 0;
    while (*pos < b->srclen && b->src[*pos] != 'e') {
      /* A dict key must be a byte string. */
      if (is_dict) {
        int k = parse_value(b, pos, depth + 1);
        if (k < 0 || b->nodes[k].type != BEN_STR)
          return -1;
        if (prev < 0)
          b->nodes[idx].first_child = k;
        else
          b->nodes[prev].next_sibling = k;
        prev = k;
      }
      int v = parse_value(b, pos, depth + 1);
      if (v < 0)
        return -1;
      if (prev < 0)
        b->nodes[idx].first_child = v;
      else
        b->nodes[prev].next_sibling = v;
      prev = v;
      pairs++;
    }
    if (*pos >= b->srclen) /* no closing 'e'          */
      return -1;
    (*pos)++; /* consume 'e'             */
    b->nodes[idx].type = is_dict ? BEN_DICT : BEN_LIST;
    b->nodes[idx].n_children = pairs; /* list: #elems, dict: #pairs */

  } else {
    return -1; /* not a bencode value      */
  }

  b->nodes[idx].raw = b->src + start;
  b->nodes[idx].rawlen = *pos - start;
  return idx;
}

int ben_parse(struct bencode *b, const uint8_t *buf, uint32_t len, struct ben_node *pool, int cap)
{
  b->src = buf;
  b->srclen = len;
  b->nodes = pool;
  b->cap = cap;
  b->count = 0;
  b->root = -1;

  uint32_t pos = 0;
  int root = parse_value(b, &pos, 0);
  if (root < 0 || pos != len) /* reject trailing junk  */
    return -1;
  b->root = root;
  return 0;
}

/* Compare a BEN_STR node's bytes to a C string (exact length match). */
static int str_eq(const struct ben_node *n, const char *key)
{
  uint32_t i = 0;
  for (; key[i]; i++)
    if (i >= n->slen || n->str[i] != (uint8_t)key[i])
      return 0;
  return i == n->slen;
}

int ben_dict_get(const struct bencode *b, int dict, const char *key)
{
  if (dict < 0 || b->nodes[dict].type != BEN_DICT)
    return -1;
  for (int k = b->nodes[dict].first_child; k >= 0;) {
    int v = b->nodes[k].next_sibling; /* value follows its key   */
    if (v < 0)
      return -1; /* malformed: key with no value */
    if (str_eq(&b->nodes[k], key))
      return v;
    k = b->nodes[v].next_sibling; /* skip to the next key    */
  }
  return -1;
}

int ben_list_count(const struct bencode *b, int list)
{
  if (list < 0 || b->nodes[list].type != BEN_LIST)
    return -1;
  return b->nodes[list].n_children;
}

int ben_list_at(const struct bencode *b, int list, int i)
{
  if (list < 0 || b->nodes[list].type != BEN_LIST || i < 0)
    return -1;
  int c = b->nodes[list].first_child;
  for (; c >= 0 && i > 0; i--)
    c = b->nodes[c].next_sibling;
  return c;
}

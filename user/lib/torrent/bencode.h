/* bencode.h — a bencode (BitTorrent metainfo) parser.
 *
 * Bencode has four types: integers (i<n>e), byte strings (<len>:<bytes>),
 * lists (l...e), and dictionaries (d...e). This parser does a single
 * recursive-descent pass over an in-memory buffer, building a flat tree of
 * nodes in a caller-supplied pool. It is zero-copy: string and "raw" spans
 * point back into the source buffer, which must outlive the tree.
 *
 * Bencode here arrives from the network (a .torrent file or a tracker reply),
 * so the parser is written to never read out of bounds and to reject
 * malformed input rather than trust it: every length is checked against the
 * remaining buffer and nesting depth is capped.
 */
#ifndef OWOS_BENCODE_H
#define OWOS_BENCODE_H

#include <stdint.h>

enum ben_type { BEN_INT, BEN_STR, BEN_LIST, BEN_DICT };

/* One parsed value. Containers chain their children with sibling indices: a
 * list's children are its elements; a dict's children are key,value,key,value
 * (keys are always BEN_STR). first_child / next_sibling are pool indices, or
 * -1 for "none". */
struct ben_node {
	uint8_t        type;          /* enum ben_type                            */
	int            first_child;   /* container: first element/pair, else -1   */
	int            next_sibling;  /* next element within the parent, else -1  */
	int            n_children;    /* list: #elements; dict: #pairs            */
	int64_t        ival;          /* BEN_INT value                            */
	const uint8_t *str;           /* BEN_STR bytes (into source)              */
	uint32_t       slen;          /* BEN_STR length                           */
	const uint8_t *raw;           /* this value's full encoding (into source) */
	uint32_t       rawlen;        /* length of that encoding                  */
};

/* A parse result: the node pool plus bookkeeping. Populated by ben_parse(). */
struct bencode {
	const uint8_t   *src;         /* the buffer that was parsed               */
	uint32_t         srclen;
	struct ben_node *nodes;       /* caller-owned pool                        */
	int              cap;         /* pool capacity (#nodes)                   */
	int              count;       /* nodes used                               */
	int              root;        /* root node index, or -1                   */
};

/* Parse buf[0..len) as exactly one bencoded value into the caller's `pool`
 * (cap nodes). Returns 0 and fills *b on success, -1 on malformed input,
 * pool overflow, or trailing garbage after the top-level value. */
int ben_parse(struct bencode *b, const uint8_t *buf, uint32_t len,
              struct ben_node *pool, int cap);

/* Look up `key` in a BEN_DICT node; returns the value's node index, or -1 if
 * `dict` is not a dict or the key is absent. */
int ben_dict_get(const struct bencode *b, int dict, const char *key);

/* List helpers: element count, and the index of the i-th element (or -1). */
int ben_list_count(const struct bencode *b, int list);
int ben_list_at(const struct bencode *b, int list, int i);

#endif /* OWOS_BENCODE_H */

/* sqlite.h — a from-scratch reader/writer for the real SQLite3 file format.
 *
 * This is a genuine SQLite database engine in miniature: sql_save() emits a
 * byte-for-byte valid SQLite3 file (the 100-byte database header, table b-tree
 * pages built bottom-up — leaves plus interior pages when a table outgrows one
 * page, the record/serial-type encoding, and varints), and sql_open() parses
 * one back. A file written here opens in the host `sqlite3` shell, and a real
 * SQLite file (UTF-8, no overflow/index pages) loads here.
 *
 * It is deliberately scoped to what an embedded app needs and what fits OwOS's
 * whole-file I/O (readfile/writefile, no random writes): an in-memory model.
 * sql_open() reads the entire file and parses it into tables and rows; you
 * mutate that model in RAM; sql_save() serialises the whole thing back out.
 * Supported on read: table b-trees (leaf + interior), records of NULL / integer
 * / IEEE-float / TEXT / BLOB columns. Not supported: index b-trees, overflow
 * pages (so a single record must fit on one page — true for everything we
 * write), the WAL, and incremental/random updates. UTF-8 text only.
 *
 * The browser uses it as a persistent cookie jar (a `cookies` table); it is a
 * general library, not cookie-specific.
 */
#ifndef OWOS_SQLITE_H
#define OWOS_SQLITE_H

#define SQL_MAX_COLS 32       /* columns per row this model carries           */

/* A column's storage class — the SQLite serial-type families collapse to these
 * four for the caller. SQL_BLOB and SQL_TEXT both carry `text`/`len`; text is
 * additionally NUL-terminated for convenience. */
enum sql_type { SQL_NULL = 0, SQL_INT = 1, SQL_TEXT = 2, SQL_BLOB = 3 };

struct sql_value {
	int        type;          /* enum sql_type                                */
	long long  i;             /* SQL_INT value                                */
	char      *text;          /* SQL_TEXT/SQL_BLOB bytes (NUL-terminated)     */
	int        len;           /* byte length of text/blob (excludes the NUL)  */
};

struct sql_row {
	long long        rowid;
	int              ncol;
	struct sql_value col[SQL_MAX_COLS];
	struct sql_row  *next;
};

struct sql_table {
	char             name[64];
	char            *sql;        /* the CREATE TABLE text stored in the schema */
	int              ncol;       /* declared column count (best-effort)        */
	long long        next_rowid; /* auto-assigned to the next inserted row     */
	struct sql_row  *rows;       /* singly linked, in rowid order              */
	struct sql_row  *tail;
	int              nrows;
	int              load_root;  /* transient: rootpage seen while loading     */
	struct sql_table *next;
};

struct sql_db {
	struct sql_table *tables;
};

/* Open `path`: parse an existing SQLite3 file into memory. A missing or empty
 * file yields a fresh empty db (ready for sql_create_table + sql_save).
 * Returns NULL only on a corrupt/unsupported file or out of memory. */
struct sql_db *sql_open(const char *path);

/* A brand-new empty in-memory database (no backing file yet). */
struct sql_db *sql_new(void);

struct sql_table *sql_table_find(struct sql_db *db, const char *name);

/* Define a table. `create_sql` is stored verbatim as its schema SQL (what the
 * host `sqlite3 .schema` shows) and `ncol` is the column count rows carry.
 * Returns the table, or NULL (duplicate name / OOM). */
struct sql_table *sql_create_table(struct sql_db *db, const char *name,
                                   const char *create_sql, int ncol);

/* Append a row of `ncol` values (deep-copied). rowid is auto-assigned. 0/-1. */
int  sql_insert(struct sql_table *t, const struct sql_value *vals, int ncol);

/* Drop every row of a table (keeps its schema). */
void sql_truncate(struct sql_table *t);

/* Serialise the whole db to a real SQLite3 file at `path`. Returns 0/-1. */
int  sql_save(struct sql_db *db, const char *path);

void sql_close(struct sql_db *db);

/* Value constructors for building rows to insert. The text variant BORROWS the
 * pointer (valid only until sql_insert copies it). */
static inline struct sql_value sqlv_int(long long v)
{ struct sql_value x; x.type = SQL_INT; x.i = v; x.text = 0; x.len = 0; return x; }
static inline struct sql_value sqlv_null(void)
{ struct sql_value x; x.type = SQL_NULL; x.i = 0; x.text = 0; x.len = 0; return x; }
struct sql_value sqlv_text(const char *s);   /* len = strlen(s); borrows s    */

#endif /* OWOS_SQLITE_H */

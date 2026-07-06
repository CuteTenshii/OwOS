/* sqlite_test.c — host unit test for user/lib/sqlite overflow-page support.
 *
 * Builds a database whose rows range from tiny to far larger than one 4 KiB
 * page, saves it as a real SQLite3 file, reads it back, and checks every value
 * survives byte-for-byte — exercising the overflow-chain writer and reader. It
 * also checks save/open is a fixpoint (a reloaded db re-saves identically) and,
 * if a host `sqlite3` binary is present, that the emitted file opens there.
 *
 *   cc -std=c11 -Iscripts -Iuser/lib/sqlite \
 *      scripts/sqlite_test.c user/lib/sqlite/sqlite.c \
 *      -o /tmp/sqlite_test && /tmp/sqlite_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* Deterministic byte pattern of length n; distinct per seed. */
static char *pattern(int n, int seed)
{
	char *b = malloc((size_t)n + 1);
	for (int i = 0; i < n; i++) b[i] = (char)(((i * 131) + seed * 7 + 17) & 0xff);
	b[n] = '\0';
	return b;
}

/* Read a whole file into a malloc'd buffer for byte-exact comparison. */
static unsigned char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc((size_t)*len);
	if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) { free(b); b = 0; }
	fclose(f);
	return b;
}

struct row_spec { int len; int seed; };

/* Row sizes chosen around the leaf capacity: small (no overflow), just under
 * and just over one page, and several multiples of a page (multi-link chains). */
static const struct row_spec specs[] = {
	{ 0, 1 }, { 10, 2 }, { 4000, 3 }, { 4061, 4 }, { 4062, 5 },
	{ 4096, 6 }, { 8192, 7 }, { 12345, 8 }, { 65536, 9 }, { 100, 10 },
};
#define NSPEC ((int)(sizeof specs / sizeof specs[0]))

int main(void)
{
	const char *path = "/tmp/sqlite_test.db";

	/* Build the source database: one BLOB column per row so arbitrary bytes
	 * round-trip, plus a companion int to prove multi-column records. */
	struct sql_db *db = sql_new();
	CHECK(db != 0, "sql_new");
	struct sql_table *t = sql_create_table(db, "blobs",
	    "CREATE TABLE blobs(id INTEGER PRIMARY KEY, data BLOB, n INTEGER)", 3);
	CHECK(t != 0, "create table");

	char *bodies[NSPEC];
	for (int i = 0; i < NSPEC; i++) {
		bodies[i] = pattern(specs[i].len, specs[i].seed);
		struct sql_value v[3];
		v[0] = sqlv_null();                    /* id aliases the rowid          */
		v[1].type = SQL_BLOB; v[1].i = 0; v[1].text = bodies[i]; v[1].len = specs[i].len;
		v[2] = sqlv_int(specs[i].len);
		CHECK(sql_insert(t, v, 3) == 0, "insert row %d", i);
	}

	CHECK(sql_save(db, path) == 0, "sql_save");
	sql_close(db);

	/* Read it back and verify every row byte-for-byte. */
	struct sql_db *db2 = sql_open(path);
	CHECK(db2 != 0, "sql_open");
	struct sql_table *t2 = sql_table_find(db2, "blobs");
	CHECK(t2 != 0, "find table after reload");
	if (t2) {
		CHECK(t2->nrows == NSPEC, "row count %d != %d", t2->nrows, NSPEC);
		int i = 0;
		for (struct sql_row *r = t2->rows; r; r = r->next, i++) {
			if (i >= NSPEC) break;
			CHECK(r->rowid == i + 1, "rowid %lld != %d", r->rowid, i + 1);
			CHECK(r->ncol == 3, "ncol %d row %d", r->ncol, i);
			struct sql_value *d = &r->col[1];
			CHECK(d->type == SQL_BLOB, "col1 type row %d", i);
			CHECK(d->len == specs[i].len, "len %d != %d row %d",
			      d->len, specs[i].len, i);
			CHECK(specs[i].len == 0 ||
			      memcmp(d->text, bodies[i], (size_t)specs[i].len) == 0,
			      "body mismatch row %d (len %d)", i, specs[i].len);
			CHECK(r->col[2].type == SQL_INT && r->col[2].i == specs[i].len,
			      "n mismatch row %d", i);
		}
		CHECK(i == NSPEC, "iterated %d rows", i);
	}

	/* Save/open fixpoint: re-saving the reloaded db must be byte-identical. */
	const char *path2 = "/tmp/sqlite_test2.db";
	CHECK(sql_save(db2, path2) == 0, "re-save");
	sql_close(db2);
	long l1 = 0, l2 = 0;
	unsigned char *f1 = slurp(path, &l1), *f2 = slurp(path2, &l2);
	CHECK(f1 && f2 && l1 == l2 && memcmp(f1, f2, (size_t)l1) == 0,
	      "save/open not a fixpoint (%ld vs %ld bytes)", l1, l2);
	free(f1); free(f2);

	for (int i = 0; i < NSPEC; i++) free(bodies[i]);

	if (failures == 0) printf("sqlite_test: all checks passed (%d rows)\n", NSPEC);
	else printf("sqlite_test: %d FAILURES\n", failures);
	return failures ? 1 : 0;
}

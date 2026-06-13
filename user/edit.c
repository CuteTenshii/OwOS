/* edit.c — a small line-oriented text editor for owofs.
 *
 * The console is line-buffered and cannot reposition its cursor (the keyboard
 * driver swallows the arrow keys, and there is no clear/move-cursor syscall),
 * so this is an `ed`-style editor: you operate on numbered lines through a
 * command prompt rather than moving a cursor around a full-screen canvas.
 *
 *   run edit <file>     open <file> (created on first write if absent)
 *
 * Commands at the `*` prompt:
 *   p          print the buffer with line numbers
 *   a          append: type lines, end with a single "." on its own line
 *   i N        insert before line N (same "." terminator)
 *   c N        change line N: the next line you type replaces it
 *   d N        delete line N
 *   w          write the buffer back to the file
 *   q          quit (refused if there are unsaved changes)
 *   q!         quit, discarding unsaved changes
 *   h          show this help
 */

#include "owos.h"

#define MAX_LINES 256
#define LINE_LEN  128
#define LOAD_MAX  (MAX_LINES * LINE_LEN)   /* upper bound on a file we'll open  */

static char lines[MAX_LINES][LINE_LEN];
static int  nlines;
static int  dirty;
static char filename[24];

static char loadbuf[LOAD_MAX];
static char savebuf[LOAD_MAX + MAX_LINES];   /* room for a '\n' after each line */

static void copy_line(char *dst, const char *src)
{
	int i = 0;
	while (i < LINE_LEN - 1 && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

/* Parse a 1-based line number out of a command argument; 0 if none/invalid. */
static int parse_line_no(const char *s)
{
	int v = 0;
	while (*s == ' ')
		s++;
	if (*s < '0' || *s > '9')
		return 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

/* Split a freshly-read file into the line array, truncating to our limits. */
static void load_lines(int n)
{
	int col = 0;
	nlines = 0;
	for (int i = 0; i < n && nlines < MAX_LINES; i++) {
		char c = loadbuf[i];
		if (c == '\n') {
			lines[nlines][col] = '\0';
			nlines++;
			col = 0;
		} else if (col < LINE_LEN - 1) {
			lines[nlines][col++] = c;
		}
		/* characters past the line limit are dropped */
	}
	/* a final line with no trailing newline */
	if (col > 0 && nlines < MAX_LINES) {
		lines[nlines][col] = '\0';
		nlines++;
	}
}

/* Insert `text` so it becomes line `at` (0-based). Returns 0, or -1 if full. */
static int insert_at(int at, const char *text)
{
	if (nlines >= MAX_LINES)
		return -1;
	for (int j = nlines; j > at; j--)
		copy_line(lines[j], lines[j - 1]);
	copy_line(lines[at], text);
	nlines++;
	dirty = 1;
	return 0;
}

static void delete_at(int at)
{
	for (int j = at; j < nlines - 1; j++)
		copy_line(lines[j], lines[j + 1]);
	nlines--;
	dirty = 1;
}

static void print_buffer(void)
{
	if (nlines == 0) {
		puts("(empty)\n");
		return;
	}
	for (int i = 0; i < nlines; i++) {
		print_uint((unsigned)(i + 1));
		puts("  ");
		puts(lines[i]);
		puts("\n");
	}
}

/* Read lines from the user, inserting each starting at `at`, until a lone ".".*/
static void input_mode(int at)
{
	char buf[LINE_LEN];
	for (;;) {
		int n = readline(buf, sizeof buf);
		if (n == 1 && buf[0] == '.')
			break;
		if (insert_at(at, buf) != 0) {
			puts("buffer full\n");
			break;
		}
		at++;
	}
}

static void save_file(void)
{
	int out = 0;
	for (int i = 0; i < nlines; i++) {
		const char *s = lines[i];
		for (int k = 0; s[k]; k++)
			savebuf[out++] = s[k];
		savebuf[out++] = '\n';
	}
	if (writefile(filename, savebuf, out) == 0) {
		puts("wrote ");
		puts(filename);
		puts(" (");
		print_uint((unsigned)nlines);
		puts(" lines)\n");
		dirty = 0;
	} else {
		puts("write failed\n");
	}
}

static void help(void)
{
	puts("commands:\n");
	puts("  p      print the buffer\n");
	puts("  a      append lines (end with '.')\n");
	puts("  i N    insert before line N\n");
	puts("  c N    change line N\n");
	puts("  d N    delete line N\n");
	puts("  w      write to file\n");
	puts("  q      quit (q! to discard changes)\n");
	puts("  h      this help\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: run edit <file>\n");
		return 1;
	}
	copy_line(filename, argv[1]);
	if (strlen(filename) > 23)            /* owofs name limit (FS_NAME_LEN) */
		filename[23] = '\0';

	int n = readfile(filename, loadbuf, LOAD_MAX);
	if (n > 0) {
		load_lines(n);
		puts("editing ");
		puts(filename);
		puts(" (");
		print_uint((unsigned)nlines);
		puts(" lines). type 'h' for help.\n");
	} else {
		nlines = 0;
		puts("new file ");
		puts(filename);
		puts(". type 'h' for help.\n");
	}

	char cmd[LINE_LEN];
	for (;;) {
		puts("* ");
		int len = readline(cmd, sizeof cmd);
		if (len <= 0)                   /* EOF on the console: just stop */
			break;

		char c = cmd[0];
		const char *arg = cmd + 1;
		int ln = parse_line_no(arg);

		if (c == 'p') {
			print_buffer();
		} else if (c == 'a') {
			input_mode(nlines);
		} else if (c == 'i') {
			if (ln < 1 || ln > nlines + 1) {
				puts("bad line number\n");
				continue;
			}
			input_mode(ln - 1);
		} else if (c == 'c') {
			if (ln < 1 || ln > nlines) {
				puts("bad line number\n");
				continue;
			}
			char buf[LINE_LEN];
			readline(buf, sizeof buf);
			copy_line(lines[ln - 1], buf);
			dirty = 1;
		} else if (c == 'd') {
			if (ln < 1 || ln > nlines) {
				puts("bad line number\n");
				continue;
			}
			delete_at(ln - 1);
		} else if (c == 'w') {
			save_file();
		} else if (c == 'q') {
			if (arg[0] == '!' || !dirty)
				break;
			puts("unsaved changes -- 'w' to save, or 'q!' to discard\n");
		} else if (c == 'h' || c == '?') {
			help();
		} else {
			puts("unknown command -- 'h' for help\n");
		}
	}
	return 0;
}

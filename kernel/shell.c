/* shell.c — line editing and a handful of built-in commands. */

#include <stddef.h>
#include <stdint.h>
#include "shell.h"
#include "vga.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "fs.h"
#include "fat.h"
#include "hfs.h"
#include "ext.h"
#include "blk.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "pci.h"
#include "usb.h"
#include "speaker.h"
#include "sb16.h"
#include "cpu.h"
#include "rtc.h"
#include "neofetch.h"
#include "loader.h"
#include "pipe.h"
#include "power.h"
#include "net.h"
#include "crypto.h"
#include "url.h"
#include "keyboard.h"
#include "wm.h"
#include "serial.h"
#include "zip.h"
#include "gzip.h"
#include "services.h"
#include "torrent.h"
#include "tracker.h"
#include "peer.h"

#define LINE_MAX 128
#define HIST_MAX 16

static char     line[LINE_MAX];
static size_t   line_len;        /* characters currently in line             */
static size_t   cursor;          /* insertion point, 0..line_len             */
static uint16_t edit_start;      /* VGA linear pos where the line text begins */
static size_t   render_len;      /* chars currently drawn for this line       */

/* Command history: a small ring browsed with Up/Down. hist_pos == hist_count
 * means "the new line being typed" (nothing recalled yet). */
static char history[HIST_MAX][LINE_MAX];
static int  hist_count;
static int  hist_pos;

/* When a generic pipe "A | B" is active, this holds A's captured output as a
 * NUL-terminated string; input-consuming builtins (grep) read from it. */
static char  *g_pipe_in;

static int streq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++; b++;
	}
	return *a == *b;
}

/* If line starts with prefix, return the pointer just past it; else NULL. */
static const char *after_prefix(const char *line, const char *prefix)
{
	while (*prefix) {
		if (*line != *prefix)
			return NULL;
		line++; prefix++;
	}
	return line;
}

static uint32_t parse_uint(const char *s)
{
	uint32_t v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (uint32_t)(*s++ - '0');
	return v;
}

static size_t str_len(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static void str_copy(char *dst, const char *src)
{
	size_t i = 0;
	for (; src[i] && i < LINE_MAX - 1; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* ------------------------------------------------------- working directory */

#define PATH_MAX 128

/* The shell's current directory, kept as a clean absolute path (no trailing
 * slash except for the root). owofs itself has no cwd -- the shell turns every
 * file argument into an absolute path before calling into the filesystem. */
#define HOME_DIR "/home/root"
static char cwd[PATH_MAX] = HOME_DIR;

/* Resolve `arg` against the current directory into a normalized absolute path
 * in out[PATH_MAX] (".", "..", and duplicate slashes collapsed). An absolute
 * arg ignores cwd. Returns 0, or -1 if the result would overflow PATH_MAX. */
static int path_resolve(const char *arg, char *out)
{
	char tmp[PATH_MAX * 2];
	int n = 0;

	if (arg[0] == '/') {                           /* absolute: start from root */
		tmp[n++] = '/';
	} else {                                        /* relative: seed with cwd   */
		for (int i = 0; cwd[i]; i++) {
			if (n >= (int)sizeof(tmp) - 2)
				return -1;
			tmp[n++] = cwd[i];
		}
		if (n == 0 || tmp[n - 1] != '/')
			tmp[n++] = '/';
	}
	for (int i = 0; arg[i]; i++) {                  /* append the argument       */
		if (n >= (int)sizeof(tmp) - 2)
			return -1;
		tmp[n++] = arg[i];
	}
	tmp[n] = '\0';

	/* Normalize: walk components onto a stack of (offset,len) into out[]. */
	int starts[PATH_MAX / 2];
	int ends[PATH_MAX / 2];
	int depth = 0;
	int i = 0;
	while (tmp[i]) {
		while (tmp[i] == '/')
			i++;
		int s = i;
		while (tmp[i] && tmp[i] != '/')
			i++;
		int len = i - s;
		if (len == 0)
			continue;
		if (len == 1 && tmp[s] == '.')
			continue;                          /* "." : stay                */
		if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
			if (depth > 0)
				depth--;                   /* ".." : pop                */
			continue;
		}
		if (depth < (int)(sizeof(starts) / sizeof(starts[0]))) {
			starts[depth] = s;
			ends[depth] = i;
			depth++;
		}
	}

	int o = 0;
	if (depth == 0) {
		out[o++] = '/';                            /* the root                  */
	} else {
		for (int d = 0; d < depth; d++) {
			if (o >= PATH_MAX - 1)
				return -1;
			out[o++] = '/';
			for (int k = starts[d]; k < ends[d]; k++) {
				if (o >= PATH_MAX - 1)
					return -1;
				out[o++] = tmp[k];
			}
		}
	}
	out[o] = '\0';
	return 0;
}

/* If drawing since the saved scroll count `before` scrolled the console, the
 * line's anchor moved up with everything else; slide edit_start to match (it
 * can't legitimately fall off the top -- a line is at most a couple of rows). */
static void edit_fix_anchor(uint32_t before)
{
	uint32_t rows = vga_scroll_count() - before;
	if (rows)
		edit_start = vga_pos_scroll_up(edit_start, rows);
}

/* Redraw the current line in place: text from edit_start, spaces to wipe any
 * leftover tail from a previously longer line, then park the cursor at the
 * insertion point. Stays correct even if the line is long enough to scroll the
 * screen mid-redraw -- edit_start is re-anchored against any scrolling. */
static void edit_redraw(void)
{
	uint32_t s0 = vga_scroll_count();
	vga_set_pos(edit_start);
	for (size_t i = 0; i < line_len; i++)
		vga_putc(line[i]);
	for (size_t i = line_len; i < render_len; i++)
		vga_putc(' ');
	render_len = line_len;
	edit_fix_anchor(s0);
	vga_set_pos((uint16_t)(edit_start + cursor));
}

/* Replace the editable line with s, cursor at the end, and redraw. */
static void edit_set(const char *s)
{
	size_t n = 0;
	while (s[n] && n < LINE_MAX - 1) {
		line[n] = s[n];
		n++;
	}
	line_len = cursor = n;
	edit_redraw();
}

static void hist_add(const char *s)
{
	if (s[0] == '\0')
		return;
	if (hist_count > 0 && streq(history[hist_count - 1], s)) {
		hist_pos = hist_count;         /* skip consecutive duplicate          */
		return;
	}
	if (hist_count == HIST_MAX) {          /* full: drop the oldest           */
		for (int i = 1; i < HIST_MAX; i++)
			str_copy(history[i - 1], history[i]);
		hist_count--;
	}
	str_copy(history[hist_count++], s);
	hist_pos = hist_count;
}

static void hist_up(void)              /* recall older entries               */
{
	if (hist_pos > 0) {
		hist_pos--;
		edit_set(history[hist_pos]);
	}
}

static void hist_down(void)            /* back toward the new line           */
{
	if (hist_pos >= hist_count)
		return;
	hist_pos++;
	edit_set(hist_pos == hist_count ? "" : history[hist_pos]);
}

static void print_prompt(void)
{
	vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
	vga_write("owos:");
	/* Show the working directory, abbreviating the home directory to "~". */
	size_t hlen = str_len(HOME_DIR);
	int in_home = 1;
	for (size_t i = 0; i < hlen; i++)
		if (cwd[i] != HOME_DIR[i]) { in_home = 0; break; }
	if (in_home && (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
		vga_putc('~');
		vga_write(cwd + hlen);                 /* the path below home, if any */
	} else {
		vga_write(cwd);
	}
	vga_write("$ ");
	vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Begin a fresh input line: draw the prompt and reset the editor state. */
static void prompt(void)
{
	print_prompt();
	edit_start = vga_get_pos();
	line_len = cursor = render_len = 0;
}

/* "help" — list every built-in. The text is data in the `builtins` table just
 * above run(), so this walks that table; the definition lives there too. */
static void cmd_help(const char *a);

static void print_kib(const char *label, uint32_t kib)
{
	vga_write(label);
	vga_write_dec(kib);
	vga_write(" KiB\n");
}

/* List every storage device the kernel brought up, with its capacity, and do a
 * read-only sanity check (read LBA 0) on the DMA-based controllers. */
/* Print a 512-byte-sector capacity as "N MiB", or "N.N GiB" once it reaches
 * 1 GiB so large disks read naturally. */
static void write_capacity(uint32_t sectors)
{
	uint32_t mib = sectors / 2048;
	if (mib >= 1024) {
		uint32_t tenths = (mib * 10) / 1024;   /* MiB -> tenths of a GiB        */
		vga_write_dec(tenths / 10);
		vga_putc('.');
		vga_write_dec(tenths % 10);
		vga_write(" GiB");
	} else {
		vga_write_dec(mib);
		vga_write(" MiB");
	}
}

static void cmd_storage(void)
{
	uint8_t buf[512];
	int n = 0;

	if (ata_present()) {
		vga_write("  IDE/ATA    ");
		vga_write(ata_model());
		vga_write("  ");
		write_capacity(ata_sectors());
		vga_write(" (PIO, primary master)\n");
		n++;
	}
	if (ahci_present()) {
		vga_write("  SATA/AHCI  ");
		vga_write(ahci_model());
		vga_write("  ");
		write_capacity(ahci_sectors());
		vga_write(" (DMA) -- read LBA0: ");
		vga_write(ahci_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (nvme_present()) {
		vga_write("  NVMe       ");
		vga_write(nvme_model());
		vga_write("  ");
		write_capacity(nvme_sectors());
		vga_write(" (ns 1) -- read LBA0: ");
		vga_write(nvme_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (!n)
		vga_write("  no storage devices detected\n");
}

static void cmd_meminfo(void)
{
	print_kib("phys total: ", pmm_total_kib());
	print_kib("phys used:  ", pmm_used_kib());
	print_kib("phys free:  ", pmm_free_kib());
	print_kib("heap used:  ", (uint32_t)(heap_used_bytes() / 1024));
	print_kib("heap free:  ", (uint32_t)(heap_free_bytes() / 1024));
}

/* Allocate two blocks (expect distinct, non-overlapping), write and read them
 * back, free the first, and confirm the next allocation reuses its address. */
static void cmd_memtest(void)
{
	char *a = kmalloc(64);
	char *b = kmalloc(64);
	if (!a || !b || a == b || (b >= a && b < a + 64)) {
		vga_write("memtest: FAIL (bad allocation)\n");
		return;
	}
	for (int i = 0; i < 64; i++) {
		a[i] = (char)('A' + (i % 26));
		b[i] = (char)('a' + (i % 26));
	}
	if (a[0] != 'A' || a[25] != 'Z' || b[0] != 'a') {
		vga_write("memtest: FAIL (data corrupted)\n");
		return;
	}
	kfree(a);
	char *c = kmalloc(64);          /* should reclaim a's slot                */
	if (c != a) {
		vga_write("memtest: FAIL (no reuse after free)\n");
		return;
	}
	kfree(b);
	kfree(c);
	vga_write("memtest: PASS\n");
}

/* Split "prog a b c" in place into argv; returns argc (argv[0] is the prog). */
static int tokenize(char *s, char **argv, int max)
{
	int argc = 0;
	while (*s && argc < max) {
		while (*s == ' ')
			*s++ = '\0';
		if (!*s)
			break;
		argv[argc++] = s;
		while (*s && *s != ' ')
			s++;
	}
	return argc;
}

/* Read argv[0] off disk and spawn it; returns pid (-1 on failure). The program
 * is looked up like a shell PATH: first relative to the current directory (or
 * as an absolute path), then in /bin. argv[0] passed to the program is left as
 * typed. */
static int spawn(int argc, char **argv)
{
	if (argc == 0)
		return -1;
	uint8_t *buf = kmalloc(64 * 1024);
	if (!buf)
		return -1;

	char path[PATH_MAX];
	int n = -1;
	if (path_resolve(argv[0], path) == 0)
		n = fs_read(path, buf, 64 * 1024);
	if (n < 0 && argv[0][0] != '/') {              /* fall back to /bin/<name>  */
		char binp[PATH_MAX];
		int j = 0;
		const char *pfx = "/bin/";
		for (int i = 0; pfx[i]; i++)
			binp[j++] = pfx[i];
		for (int i = 0; argv[0][i] && j < PATH_MAX - 1; i++)
			binp[j++] = argv[0][i];
		binp[j] = '\0';
		n = fs_read(binp, buf, 64 * 1024);
	}

	int pid = (n < 0) ? -1 : exec_program(buf, (uint32_t)n, argc, argv);
	if (pid >= 0)
		sched_set_cwd(pid, cwd);   /* child resolves relative paths from here */
	kfree(buf);
	return pid;
}

static void wait_reap(int pid)
{
	int status;
	while (!sched_try_reap(pid, &status)) {
		/* While a foreground child runs, the main loop is parked here and isn't
		 * draining the keyboard. If a GUI window holds focus (e.g. a ring-3 app
		 * the child spawned), pump input to it; otherwise leave keystrokes in
		 * the ring for a console program's own read()/readline. */
		if (wm_focus_takes_keys()) {
			char c;
			while (keyboard_poll(&c))
				wm_feed_key(c);
			while (serial_poll(&c))
				wm_feed_key(c);
		}
		__asm__ volatile ("sti; hlt");
	}
	vga_write("[pid ");
	vga_write_dec((uint32_t)pid);
	vga_write(" exited, status ");
	vga_write_dec((uint32_t)status);
	vga_write("]\n");
}

static void cmd_run(char *args)
{
	/* A pipeline "a | b" wires a's stdout to b's stdin through a kernel pipe. */
	char *bar = args;
	while (*bar && *bar != '|')
		bar++;

	if (*bar == '|') {
		*bar = '\0';
		char *lhs = args, *rhs = bar + 1;
		char *av1[8], *av2[8];
		int ac1 = tokenize(lhs, av1, 8);
		int ac2 = tokenize(rhs, av2, 8);
		if (ac1 == 0 || ac2 == 0) {
			vga_write("usage: run <a> | <b>\n");
			return;
		}
		int id = pipe_alloc();
		if (id < 0) {
			vga_write("out of pipes\n");
			return;
		}
		/* Set both up before either can be scheduled. */
		__asm__ volatile ("cli");
		int p1 = spawn(ac1, av1);
		int p2 = spawn(ac2, av2);
		if (p1 >= 0)
			sched_set_pipe_fd(p1, 1, 1, id);   /* a's stdout -> pipe write     */
		if (p2 >= 0)
			sched_set_pipe_fd(p2, 0, 0, id);   /* b's stdin  -> pipe read      */
		__asm__ volatile ("sti");

		if (p1 < 0 || p2 < 0) {
			vga_write("pipeline: bad program\n");
			return;
		}
		wait_reap(p1);
		wait_reap(p2);
		return;
	}

	char *argv[8];
	int argc = tokenize(args, argv, 8);
	if (argc == 0) {
		vga_write("usage: run <file> [args]\n");
		return;
	}
	int pid = spawn(argc, argv);
	if (pid < 0) {
		vga_write("no such file / not a valid program\n");
		return;
	}
	wait_reap(pid);
}

/* Does haystack contain needle as a substring? Empty needle matches anything. */
static int str_contains(const char *haystack, const char *needle)
{
	if (!*needle)
		return 1;
	for (; *haystack; haystack++) {
		const char *h = haystack, *n = needle;
		while (*h && *n && *h == *n) {
			h++; n++;
		}
		if (!*n)
			return 1;
	}
	return 0;
}

/* Print the lines of a writable, NUL-terminated buffer that contain pattern,
 * walking it in place (each line is NUL-terminated as we go). */
static void grep_emit(const char *pattern, char *buf)
{
	char *p = buf;
	while (*p) {
		char *line_start = p;
		while (*p && *p != '\n')
			p++;
		int had_newline = (*p == '\n');
		*p = '\0';
		if (str_contains(line_start, pattern)) {
			vga_write(line_start);
			vga_putc('\n');
		}
		if (had_newline)
			p++;
	}
}

/* "grep <pattern> [file]" — print lines containing <pattern> (plain substring,
 * no regex). With a file it greps owofs; with no file it greps piped input,
 * so the one command serves both "grep p f" and "<cmd> | grep p". */
static void cmd_grep(const char *arg)
{
	char pattern[LINE_MAX];
	int i = 0;
	while (arg[i] && arg[i] != ' ' && i < (int)sizeof(pattern) - 1) {
		pattern[i] = arg[i];
		i++;
	}
	pattern[i] = '\0';
	while (arg[i] == ' ')
		i++;
	const char *name = arg + i;

	if (pattern[0] == '\0') {
		vga_write("usage: grep <pattern> <file>  (or: <cmd> | grep <pattern>)\n");
		return;
	}

	if (name[0] != '\0') {                  /* file mode                      */
		char *buf = kmalloc(4096);
		if (!buf)
			return;
		int n = fs_read(name, buf, 4095);
		if (n < 0) {
			vga_write("no such file\n");
		} else {
			buf[n] = '\0';
			grep_emit(pattern, buf);
		}
		kfree(buf);
	} else if (g_pipe_in) {                 /* piped mode                     */
		grep_emit(pattern, g_pipe_in);
	} else {
		vga_write("usage: grep <pattern> <file>  (or: <cmd> | grep <pattern>)\n");
	}
}

static void cmd_cat(const char *name)
{
	char rpath[PATH_MAX];
	if (path_resolve(name, rpath) != 0) {
		vga_write("cat: path too long\n");
		return;
	}
	char *buf = kmalloc(4096);
	if (!buf)
		return;
	int n = fs_read(rpath, buf, 4095);
	if (n < 0) {
		vga_write("no such file\n");
	} else {
		buf[n] = '\0';
		vga_write(buf);
		if (n > 0 && buf[n - 1] != '\n')
			vga_putc('\n');
	}
	kfree(buf);
}

/* Load a .torrent from owofs into a freshly kmalloc'd buffer and parse it.
 * Returns the buffer (caller frees) with *t filled, or NULL on any failure
 * (a message is printed). On success *t owns a node pool freed by
 * torrent_free(). */
static uint8_t *load_torrent(const char *file, struct torrent *t)
{
	int sz = fs_size(file);
	if (sz < 0) {
		vga_write("no such file\n");
		return NULL;
	}
	uint8_t *buf = kmalloc(sz ? (uint32_t)sz : 1);
	if (!buf) {
		vga_write("torrent: out of memory\n");
		return NULL;
	}
	int n = fs_read(file, buf, (uint32_t)sz);
	if (n < 0 || torrent_parse(t, buf, (uint32_t)n) != 0) {
		vga_write("torrent: not a valid .torrent file\n");
		kfree(buf);
		return NULL;
	}
	return buf;
}

/* Parse a dotted-quad "a.b.c.d" into ip[4]; returns the pointer past it (at
 * the next space or NUL), or NULL on a malformed address. */
static const char *parse_ip4(const char *s, uint8_t ip[4])
{
	for (int o = 0; o < 4; o++) {
		if (*s < '0' || *s > '9')
			return NULL;
		uint32_t v = 0;
		while (*s >= '0' && *s <= '9')
			v = v * 10 + (uint32_t)(*s++ - '0');
		if (v > 255)
			return NULL;
		ip[o] = (uint8_t)v;
		if (o < 3) {
			if (*s != '.')
				return NULL;
			s++;
		}
	}
	return s;
}

/* torrent peer <file> <ip> <port> — connect to one peer and leech piece 0. */
static void cmd_torrent_peer(const char *arg)
{
	char file[24];
	int fn = 0;
	while (*arg == ' ') arg++;
	while (*arg && *arg != ' ' && fn < (int)sizeof(file) - 1)
		file[fn++] = *arg++;
	file[fn] = '\0';
	while (*arg == ' ') arg++;

	uint8_t ip[4];
	const char *after = parse_ip4(arg, ip);
	if (fn == 0 || !after) {
		vga_write("usage: torrent peer <file> <ip> <port>\n");
		return;
	}
	while (*after == ' ') after++;
	uint16_t port = (uint16_t)parse_uint(after);
	if (port == 0) {
		vga_write("usage: torrent peer <file> <ip> <port>\n");
		return;
	}

	struct torrent t;
	uint8_t *buf = load_torrent(file, &t);
	if (!buf)
		return;

	uint8_t *piece = kmalloc(t.piece_length ? (uint32_t)t.piece_length : 1);
	if (!piece) {
		vga_write("torrent: out of memory\n");
		torrent_free(&t);
		kfree(buf);
		return;
	}
	int n = peer_fetch_piece(&t, ip, port, 0, piece);
	if (n > 0) {
		vga_write("got piece 0: ");
		vga_write_dec((uint32_t)n);
		vga_write(" bytes\n");
	}
	kfree(piece);
	torrent_free(&t);
	kfree(buf);
}

/* Byte length of piece `idx` (the final piece is short). */
static uint32_t piece_size_of(const struct torrent *t, uint32_t idx)
{
	uint64_t off = (uint64_t)idx * (uint64_t)t->piece_length;
	if (idx == t->n_pieces - 1)
		return (uint32_t)((uint64_t)t->total_length - off);
	return (uint32_t)t->piece_length;
}

/* torrent get <file> — download a whole single-file torrent concurrently from
 * several peers at once, verifying each piece's SHA-1 into a pre-allocated
 * output file. */
static void cmd_torrent_get(const char *file)
{
	struct torrent t;
	uint8_t *buf = load_torrent(file, &t);
	if (!buf)
		return;
	if (t.multi_file) {
		vga_write("get: multi-file torrents not supported yet\n");
		torrent_free(&t);
		kfree(buf);
		return;
	}

	/* Output file = the torrent's suggested name (owofs caps it at 19 chars). */
	char outname[20];
	uint32_t nl = t.name_len < (uint32_t)sizeof(outname) - 1
	            ? t.name_len : (uint32_t)sizeof(outname) - 1;
	for (uint32_t i = 0; i < nl; i++)
		outname[i] = (char)t.name[i];
	outname[nl] = '\0';

	struct tracker_peer peers[32];
	int interval = 0;
	int npeers = tracker_announce(t.announce, t.infohash, t.total_length,
	                              peers, 32, &interval);
	if (npeers <= 0) {
		vga_write("get: no peers available\n");
		torrent_free(&t);
		kfree(buf);
		return;
	}

	if (fs_prealloc(outname, (uint32_t)t.total_length) != 0) {
		vga_write("get: cannot allocate output file\n");
		torrent_free(&t);
		kfree(buf);
		return;
	}

	/* Concurrent download: open up to GET_MAX_CONN connections and service them
	 * all from this one thread with non-blocking pumps, so pieces stream from
	 * several peers at once. Each connection has its own piece buffer. */
	#define GET_MAX_CONN 4
	int nconn = npeers < GET_MAX_CONN ? npeers : GET_MAX_CONN;

	peer_conn_t *pc[GET_MAX_CONN]   = { 0 };
	uint8_t     *pbuf[GET_MAX_CONN] = { 0 };
	uint8_t *done_f = kmalloc(t.n_pieces ? t.n_pieces : 1);   /* piece completed */
	uint8_t *inflt  = kmalloc(t.n_pieces ? t.n_pieces : 1);   /* assigned to a conn */
	int alloc_ok = (done_f && inflt);
	for (int i = 0; i < nconn && alloc_ok; i++) {
		pbuf[i] = kmalloc(t.piece_length ? (uint32_t)t.piece_length : 1);
		if (!pbuf[i]) alloc_ok = 0;
	}
	if (!alloc_ok) {
		vga_write("get: out of memory\n");
		for (int i = 0; i < nconn; i++) kfree(pbuf[i]);
		kfree(done_f); kfree(inflt);
		torrent_free(&t);
		kfree(buf);
		return;
	}
	for (uint32_t i = 0; i < t.n_pieces; i++)
		done_f[i] = inflt[i] = 0;

	int live = 0;
	for (int i = 0; i < nconn; i++) {
		pc[i] = peer_connect(&t, peers[i].ip, peers[i].port);
		if (pc[i]) live++;
	}
	if (live == 0)
		vga_write("get: could not connect to any peer\n");

	/* Assign the next unstarted piece this connection can serve; idle if none. */
	#define ASSIGN(i) do {                                                     \
		int _a = 0;                                                        \
		for (uint32_t _p = 0; _p < t.n_pieces; _p++)                       \
			if (!done_f[_p] && !inflt[_p] && peer_has_piece(pc[i], _p)) {\
				if (peer_start_piece(pc[i], &t, _p, pbuf[i]) == 0) {\
					inflt[_p] = 1; _a = 1; break;              \
				}                                                  \
			}                                                          \
		(void)_a;                                                          \
	} while (0)

	for (int i = 0; i < nconn; i++)
		if (pc[i]) ASSIGN(i);

	uint32_t done = 0;
	int disk_fail = 0;
	while (done < t.n_pieces && live > 0 && !disk_fail) {
		int progressed = 0;
		for (int i = 0; i < nconn; i++) {
			if (!pc[i])
				continue;
			int r = peer_pump(pc[i], &t);
			if (r == PEER_WORKING)
				continue;
			progressed = 1;
			uint32_t idx = peer_piece(pc[i]);
			if (r == PEER_PIECE) {
				if (fs_write_at(outname, idx * (uint32_t)t.piece_length,
				                pbuf[i], piece_size_of(&t, idx)) < 0) {
					disk_fail = 1;
					break;
				}
				done_f[idx] = 1; done++;
				vga_putc('.');
				ASSIGN(i);                       /* next piece on this conn */
			} else if (r == PEER_LACKS) {
				inflt[idx] = 0;                  /* free it for another peer */
				ASSIGN(i);
			} else {                                 /* PEER_DEAD              */
				inflt[idx] = 0;                  /* requeue its piece        */
				peer_disconnect(pc[i]);
				pc[i] = NULL;
				live--;
			}
		}
		/* If every live connection is idle (nothing left it can serve), stop. */
		int any_active = 0;
		for (int i = 0; i < nconn; i++)
			if (pc[i] && peer_busy(pc[i])) any_active = 1;
		if (!any_active && !progressed)
			break;
		if (!progressed)
			__asm__ volatile ("hlt");                /* wait for the next IRQ */
	}

	if (done)
		vga_putc('\n');
	for (int i = 0; i < nconn; i++) {
		if (pc[i]) peer_disconnect(pc[i]);
		kfree(pbuf[i]);
	}
	kfree(done_f); kfree(inflt);

	if (disk_fail)
		vga_write("get: disk write failed\n");

	if (done == t.n_pieces) {
		vga_write("get: complete -- ");
		vga_write_dec((uint32_t)t.total_length);
		vga_write(" bytes verified and saved to ");
		vga_write(outname);
		vga_putc('\n');
	} else {
		vga_write("get: incomplete (");
		vga_write_dec(done);
		vga_write("/");
		vga_write_dec(t.n_pieces);
		vga_write(" pieces)\n");
	}

	torrent_free(&t);
	kfree(buf);
}

/* torrent info <file>   — parse a .torrent and print its metainfo + infohash.
 * torrent peers <file>  — announce to its tracker and list the returned peers.
 * torrent peer <f> <ip> <port> — leech piece 0 from one peer, verify its SHA-1.
 * torrent get <file>    — download the whole torrent to disk (single-file).
 * torrent test          — parse an embedded known-good torrent (offline).
 * torrent peers test     — tracker reply parsing self-check (offline).         */
static void cmd_torrent(const char *arg)
{
	if (streq(arg, "test")) {
		torrent_selftest();
		return;
	}
	if (streq(arg, "peers test")) {
		tracker_selftest();
		return;
	}
	const char *parg = after_prefix(arg, "peer ");
	if (parg) {
		cmd_torrent_peer(parg);
		return;
	}
	const char *garg = after_prefix(arg, "get ");
	if (garg && garg[0]) {
		cmd_torrent_get(garg);
		return;
	}

	const char *file = after_prefix(arg, "info ");
	const char *pfile = after_prefix(arg, "peers ");
	if ((!file || file[0] == '\0') && (!pfile || pfile[0] == '\0')) {
		vga_write("usage: torrent info <file> | torrent peers <file> "
		          "| torrent peer <file> <ip> <port> | torrent test\n");
		return;
	}

	struct torrent t;
	uint8_t *buf = load_torrent(file ? file : pfile, &t);
	if (!buf)
		return;

	if (file) {
		torrent_print(&t);
	} else {
		struct tracker_peer peers[32];
		int interval = 0;
		int cnt = tracker_announce(t.announce, t.infohash, t.total_length,
		                           peers, 32, &interval);
		if (cnt >= 0)
			tracker_print(peers, cnt, interval);
	}
	torrent_free(&t);
	kfree(buf);
}

static void wr2(uint32_t v)
{
	vga_putc('0' + (char)((v / 10) % 10));
	vga_putc('0' + (char)(v % 10));
}

/* url <string> — parse a URL into its parts and rebuild it (parser self-demo). */
static void cmd_url(const char *arg)
{
	struct url u;
	if (!url_parse(arg, &u)) {
		vga_write("url: malformed URL\n");
		return;
	}
	vga_write("  scheme:   "); vga_write(u.scheme[0] ? u.scheme : "(none)"); vga_putc('\n');
	if (u.userinfo[0]) { vga_write("  userinfo: "); vga_write(u.userinfo); vga_putc('\n'); }
	vga_write("  host:     "); vga_write(u.host); vga_putc('\n');
	vga_write("  port:     "); vga_write_dec(u.port);
	vga_write(u.has_port ? "\n" : " (default)\n");
	vga_write("  path:     "); vga_write(u.path[0] ? u.path : "(empty)"); vga_putc('\n');
	if (u.has_query)    { vga_write("  query:    "); vga_write(u.query); vga_putc('\n'); }
	if (u.has_fragment) { vga_write("  fragment: "); vga_write(u.fragment); vga_putc('\n'); }

	char rebuilt[640];
	if (url_build(&u, rebuilt, sizeof(rebuilt)) >= 0) {
		vga_write("  rebuilt:  "); vga_write(rebuilt); vga_putc('\n');
	}
}

static void cmd_date(void)
{
	struct rtc_time t;
	rtc_now(&t);
	vga_write_dec(t.year); vga_putc('-'); wr2(t.month); vga_putc('-'); wr2(t.day);
	vga_putc(' ');
	wr2(t.hour); vga_putc(':'); wr2(t.min); vga_putc(':'); wr2(t.sec);
	vga_putc('\n');
}

static void cmd_cpuinfo(void)
{
	char vendor[13];
	char brand[49];
	char flags[256];
	struct cpu_signature sig;
	uint32_t phys, virt;

	cpu_vendor(vendor);
	cpu_brand(brand);
	cpu_signature(&sig);
	cpu_address_bits(&phys, &virt);
	cpu_features(flags, sizeof(flags));

	vga_write("vendor: "); vga_write(vendor); vga_putc('\n');
	vga_write("model:  "); vga_write(brand);  vga_putc('\n');
	vga_write("family: ");  vga_write_dec(sig.family);
	vga_write("  model: "); vga_write_dec(sig.model);
	vga_write("  stepping: "); vga_write_dec(sig.stepping); vga_putc('\n');
	vga_write("address: "); vga_write_dec(phys);
	vga_write(" bits phys, "); vga_write_dec(virt); vga_write(" bits virt\n");
	uint32_t mhz = cpu_mhz();
	if (mhz) {
		vga_write("clock:  ~"); vga_write_dec(mhz); vga_write(" MHz\n");
	}
	vga_write("flags:  "); vga_write(flags); vga_putc('\n');
}

/* System identity, the fields a POSIX uname reports. */
#define UNAME_SYSNAME  "OwOS"                 /* -s kernel name              */
#define UNAME_NODENAME "owos"                 /* -n network node name        */
#define UNAME_RELEASE  "0.1"                  /* -r kernel release           */
#define UNAME_MACHINE  "i386"                 /* -m hardware                 */

/* "uname [-asnrmo]" — print system identity. Bare/-s is the kernel name;
 * -a is everything in the conventional sysname/nodename/release/machine order. */
static void cmd_uname(const char *arg)
{
	while (*arg == ' ')
		arg++;

	char flag = (arg[0] == '-') ? arg[1] : 's';
	switch (flag) {
	case 'a':
		vga_write(UNAME_SYSNAME);  vga_putc(' ');
		vga_write(UNAME_NODENAME); vga_putc(' ');
		vga_write(UNAME_RELEASE);  vga_putc(' ');
		vga_write(UNAME_MACHINE);  vga_putc('\n');
		break;
	case 's':
	case 'o': vga_write(UNAME_SYSNAME);  vga_putc('\n'); break;
	case 'n': vga_write(UNAME_NODENAME); vga_putc('\n'); break;
	case 'r': vga_write(UNAME_RELEASE);  vga_putc('\n'); break;
	case 'm': vga_write(UNAME_MACHINE);  vga_putc('\n'); break;
	default:
		vga_write("usage: uname [-asnrmo]\n");
		break;
	}
}

/* "whoami" — OwOS is single-user, so the shell always runs as root. */
static void cmd_whoami(void)
{
	vga_write("root\n");
}

/* A shell built-in is a row in the `builtins` table defined just above run():
 * a name, a handler (called with the argument string — "" when none), and a
 * one-line help string. fn == NULL marks a documentation-only pseudo-entry
 * (curl, the pipe/redirect notes) that `help` prints but dispatch skips. The
 * table is the single source of truth for dispatch, `which`, and `help`. It
 * lives below because it must follow every handler it points at. */
typedef void (*cmd_fn)(const char *arg);
struct builtin {
	const char *name;
	cmd_fn      fn;
	const char *help;
};

static int is_builtin(const char *name);

/* "which <name>..." — for each name, report whether it resolves to a shell
 * built-in or to a runnable file in owofs (checked in that order, as run()
 * would). Prints a line per name; unresolved names get a "no <name>" notice. */
static void cmd_which(char *arg)
{
	char *argv[8];
	int argc = tokenize(arg, argv, 8);
	if (argc == 0) {
		vga_write("usage: which <name>...\n");
		return;
	}
	for (int i = 0; i < argc; i++) {
		const char *name = argv[i];
		char rpath[PATH_MAX], binp[PATH_MAX];
		int j = 0;
		const char *pfx = "/bin/";
		for (int k = 0; pfx[k]; k++)
			binp[j++] = pfx[k];
		for (int k = 0; name[k] && j < PATH_MAX - 1; k++)
			binp[j++] = name[k];
		binp[j] = '\0';
		if (is_builtin(name)) {
			vga_write(name);
			vga_write(": shell built-in\n");
		} else if (path_resolve(name, rpath) == 0 && fs_size(rpath) >= 0) {
			vga_write(name); vga_write(": "); vga_write(rpath);
			vga_write(" (file)\n");
		} else if (name[0] != '/' && fs_size(binp) >= 0) {
			vga_write(name); vga_write(": "); vga_write(binp);
			vga_write(" (file)\n");
		} else {
			vga_write("which: no ");
			vga_write(name);
			vga_write(" in (built-ins, owofs)\n");
		}
	}
}

/* "loadkeys <layout>" — switch the keyboard layout (us, fr). */
static void cmd_loadkeys(const char *arg)
{
	while (*arg == ' ')
		arg++;
	if (arg[0] == '\0') {
		vga_write("usage: loadkeys <us|fr>\n");
		return;
	}
	if (keyboard_set_layout(arg) == 0) {
		vga_write("keyboard layout: ");
		vga_write(arg);
		vga_putc('\n');
	} else {
		vga_write("loadkeys: unknown layout (try us or fr)\n");
	}
}

/* "write <name> <text...>" — text is everything after the first space. */
static void cmd_write(const char *arg)
{
	char name[24];
	int i = 0;
	while (arg[i] && arg[i] != ' ' && i < (int)sizeof(name) - 1) {
		name[i] = arg[i];
		i++;
	}
	name[i] = '\0';
	const char *text = (arg[i] == ' ') ? arg + i + 1 : arg + i;

	if (name[0] == '\0') {
		vga_write("usage: write <file> <text>\n");
		return;
	}
	char rpath[PATH_MAX];
	if (path_resolve(name, rpath) != 0) {
		vga_write("write: path too long\n");
		return;
	}
	if (fs_create(rpath, text, (uint32_t)str_len(text)) == 0) {
		vga_write("wrote ");
		vga_write(rpath);
		vga_putc('\n');
	} else {
		vga_write("write failed\n");
	}
}

/* Forward decl: parse_two is defined just below, but cp/mv use it. */
static void parse_two(const char *arg, char *a, char *b, int cap);

/* If `path` is an existing directory, extend it with the last component of
 * `src` so "cp f /dir" lands at "/dir/f" -- the usual cp/mv convenience. */
static void into_dir(char *path, const char *src)
{
	if (fs_isdir(path) != 1)
		return;
	const char *base = src;
	for (const char *p = src; *p; p++)
		if (*p == '/' && p[1])
			base = p + 1;
	int n = (int)str_len(path);
	if (n == 0 || path[n - 1] != '/')
		path[n++] = '/';
	for (int i = 0; base[i] && n < PATH_MAX - 1; i++)
		path[n++] = base[i];
	path[n] = '\0';
}

/* "cd [dir]" — change the working directory (no arg -> home). */
static void cmd_cd(const char *arg)
{
	while (*arg == ' ')
		arg++;
	const char *a = (*arg == '\0') ? HOME_DIR : arg;
	char target[PATH_MAX];
	if (path_resolve(a, target) != 0) {
		vga_write("cd: path too long\n");
		return;
	}
	int d = fs_isdir(target);
	if (d < 0) {
		vga_write("cd: no such directory: "); vga_write(a); vga_putc('\n');
		return;
	}
	if (d == 0) {
		vga_write("cd: not a directory: "); vga_write(a); vga_putc('\n');
		return;
	}
	int i = 0;
	for (; target[i]; i++)
		cwd[i] = target[i];
	cwd[i] = '\0';
}

/* "pwd" — print the working directory. */
static void cmd_pwd(void)
{
	vga_write(cwd);
	vga_putc('\n');
}

/* "cp <src> <dst>" — copy a file's contents into a new (or overwritten) file.
 * If <dst> is a directory, the copy lands inside it under <src>'s basename. */
static void cmd_cp(const char *arg)
{
	char src[PATH_MAX], dst[PATH_MAX], rsrc[PATH_MAX], rdst[PATH_MAX];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0' || dst[0] == '\0') {
		vga_write("usage: cp <src> <dst>\n");
		return;
	}
	if (path_resolve(src, rsrc) != 0 || path_resolve(dst, rdst) != 0) {
		vga_write("cp: path too long\n");
		return;
	}
	into_dir(rdst, rsrc);
	if (streq(rsrc, rdst)) {
		vga_write("cp: source and destination are the same\n");
		return;
	}
	int sz = fs_size(rsrc);
	if (sz < 0) {
		vga_write("cp: no such file: "); vga_write(src); vga_putc('\n');
		return;
	}
	uint8_t *buf = kmalloc(sz ? (uint32_t)sz : 1);
	if (!buf) {
		vga_write("cp: out of memory\n");
		return;
	}
	int n = fs_read(rsrc, buf, (uint32_t)sz);
	if (n < 0)
		vga_write("cp: read failed\n");
	else if (fs_create(rdst, buf, (uint32_t)n) != 0)
		vga_write("cp: write failed (disk or directory full?)\n");
	else {
		vga_write(rsrc); vga_write(" -> "); vga_write(rdst); vga_putc('\n');
	}
	kfree(buf);
}

/* "mv <src> <dst>" — rename/move a file or directory (overwrites a file dst).
 * If <dst> is a directory, the source moves inside it under its basename. */
static void cmd_mv(const char *arg)
{
	char src[PATH_MAX], dst[PATH_MAX], rsrc[PATH_MAX], rdst[PATH_MAX];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0' || dst[0] == '\0') {
		vga_write("usage: mv <src> <dst>\n");
		return;
	}
	if (path_resolve(src, rsrc) != 0 || path_resolve(dst, rdst) != 0) {
		vga_write("mv: path too long\n");
		return;
	}
	into_dir(rdst, rsrc);
	if (streq(rsrc, rdst))
		return;                                 /* a no-op, like real mv            */
	if (fs_rename(rsrc, rdst) != 0) {
		vga_write("mv: failed (no such source, or illegal move)\n");
		return;
	}
	vga_write(rsrc); vga_write(" -> "); vga_write(rdst); vga_putc('\n');
}

/* "rm <file> [file...]" — delete one or more files (sectors leak). */
static void cmd_rm(const char *arg)
{
	char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
	parse_two(arg, name, extra, (int)sizeof(name));
	if (name[0] == '\0') {
		vga_write("usage: rm <file>\n");
		return;
	}
	if (path_resolve(name, rpath) != 0) {
		vga_write("rm: path too long\n");
	} else {
		int r = fs_delete(rpath);
		if (r == 0) {
			vga_write("removed "); vga_write(rpath); vga_putc('\n');
		} else if (r == -2) {
			vga_write("rm: is a directory (use rmdir): ");
			vga_write(name); vga_putc('\n');
		} else {
			vga_write("rm: no such file: "); vga_write(name); vga_putc('\n');
		}
	}
	if (extra[0])                                   /* a courtesy for "rm a b"  */
		cmd_rm(extra);
}

/* "mkdir <dir>" — create a directory. */
static void cmd_mkdir(const char *arg)
{
	char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
	parse_two(arg, name, extra, (int)sizeof(name));
	if (name[0] == '\0') {
		vga_write("usage: mkdir <dir>\n");
		return;
	}
	if (path_resolve(name, rpath) != 0)
		vga_write("mkdir: path too long\n");
	else if (fs_mkdir(rpath) == 0) {
		vga_write("created "); vga_write(rpath); vga_putc('\n');
	} else
		vga_write("mkdir: cannot create (exists, bad path, or directory full)\n");
	if (extra[0])
		cmd_mkdir(extra);
}

/* "rmdir <dir>" — remove an empty directory. */
static void cmd_rmdir(const char *arg)
{
	char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
	parse_two(arg, name, extra, (int)sizeof(name));
	if (name[0] == '\0') {
		vga_write("usage: rmdir <dir>\n");
		return;
	}
	if (path_resolve(name, rpath) != 0) {
		vga_write("rmdir: path too long\n");
	} else {
		int r = fs_rmdir(rpath);
		if (r == 0) {
			vga_write("removed "); vga_write(rpath); vga_putc('\n');
		} else if (r == -2) {
			vga_write("rmdir: not a directory: "); vga_write(name); vga_putc('\n');
		} else if (r == -3) {
			vga_write("rmdir: directory not empty: "); vga_write(name); vga_putc('\n');
		} else {
			vga_write("rmdir: no such directory: "); vga_write(name); vga_putc('\n');
		}
	}
	if (extra[0])
		cmd_rmdir(extra);
}

/* Copy up to two space-separated tokens out of arg into a and b (each `cap`
 * bytes, NUL-terminated). b ends up empty when only one token is present. */
static void parse_two(const char *arg, char *a, char *b, int cap)
{
	int i = 0;
	while (*arg == ' ')
		arg++;
	while (*arg && *arg != ' ' && i < cap - 1)
		a[i++] = *arg++;
	a[i] = '\0';
	while (*arg == ' ')
		arg++;
	i = 0;
	while (*arg && *arg != ' ' && i < cap - 1)
		b[i++] = *arg++;
	b[i] = '\0';
}

/* "zip <src> [dst]" — LZSS-compress a file. dst defaults to "<src>.z". */
static void cmd_zip(const char *arg)
{
	char src[24], dst[24];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0') {
		vga_write("usage: zip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		vga_write("no such file\n");
		return;
	}
	if (dst[0] == '\0') {                   /* derive <src>.z                   */
		size_t n = str_len(src);
		if (n + 2 >= sizeof(dst)) {
			vga_write("zip: name too long, pass an explicit dst\n");
			return;
		}
		str_copy(dst, src);
		dst[n] = '.'; dst[n + 1] = 'z'; dst[n + 2] = '\0';
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	uint8_t *out = kmalloc(zip_bound((uint32_t)sz));
	if (!in || !out) {
		vga_write("zip: out of memory\n");
		kfree(in); kfree(out);
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int clen = (n < 0) ? -1 :
		zip_compress(in, (uint32_t)n, out, zip_bound((uint32_t)sz));
	if (clen < 0) {
		vga_write("zip: compression failed\n");
	} else if (fs_create(dst, out, (uint32_t)clen) != 0) {
		vga_write("zip: write failed\n");
	} else {
		vga_write(src); vga_write(" -> "); vga_write(dst);
		vga_write(" ("); vga_write_dec((uint32_t)n);
		vga_write(" -> "); vga_write_dec((uint32_t)clen);
		vga_write(" bytes)\n");
	}
	kfree(in); kfree(out);
}

/* "unzip <src> [dst]" — restore a zipped file. dst defaults to <src> minus a
 * trailing ".z"; without that suffix an explicit dst is required. */
static void cmd_unzip(const char *arg)
{
	char src[24], dst[24];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0') {
		vga_write("usage: unzip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		vga_write("no such file\n");
		return;
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	if (!in) {
		vga_write("unzip: out of memory\n");
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int orig = (n < 0) ? -1 : zip_orig_size(in, (uint32_t)n);
	if (orig < 0) {
		vga_write("unzip: not an OwOS zip file\n");
		kfree(in);
		return;
	}
	if (dst[0] == '\0') {                   /* strip a trailing ".z"            */
		size_t l = str_len(src);
		if (l > 2 && src[l - 2] == '.' && src[l - 1] == 'z') {
			str_copy(dst, src);
			dst[l - 2] = '\0';
		} else {
			vga_write("usage: unzip <src> [dst]\n");
			kfree(in);
			return;
		}
	}

	uint8_t *out = kmalloc(orig ? (uint32_t)orig : 1);
	if (!out) {
		vga_write("unzip: out of memory\n");
		kfree(in);
		return;
	}
	int dlen = zip_decompress(in, (uint32_t)n, out, (uint32_t)orig);
	if (dlen < 0) {
		vga_write("unzip: corrupt archive\n");
	} else if (fs_create(dst, out, (uint32_t)dlen) != 0) {
		vga_write("unzip: write failed\n");
	} else {
		vga_write(src); vga_write(" -> "); vga_write(dst);
		vga_write(" ("); vga_write_dec((uint32_t)dlen);
		vga_write(" bytes)\n");
	}
	kfree(in); kfree(out);
}

/* "gzip <src> [dst]" — compress a file to standard gzip. dst defaults to
 * "<src>.gz"; the output is readable by host gunzip. */
static void cmd_gzip(const char *arg)
{
	char src[24], dst[24];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0') {
		vga_write("usage: gzip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		vga_write("no such file\n");
		return;
	}
	if (dst[0] == '\0') {                   /* derive <src>.gz                  */
		size_t n = str_len(src);
		if (n + 3 >= sizeof(dst)) {
			vga_write("gzip: name too long, pass an explicit dst\n");
			return;
		}
		str_copy(dst, src);
		dst[n] = '.'; dst[n + 1] = 'g'; dst[n + 2] = 'z'; dst[n + 3] = '\0';
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	uint8_t *out = kmalloc(gzip_bound((uint32_t)sz));
	if (!in || !out) {
		vga_write("gzip: out of memory\n");
		kfree(in); kfree(out);
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int clen = (n < 0) ? -1 :
		gzip_compress(in, (uint32_t)n, out, gzip_bound((uint32_t)sz));
	if (clen < 0) {
		vga_write("gzip: compression failed\n");
	} else if (fs_create(dst, out, (uint32_t)clen) != 0) {
		vga_write("gzip: write failed\n");
	} else {
		vga_write(src); vga_write(" -> "); vga_write(dst);
		vga_write(" ("); vga_write_dec((uint32_t)n);
		vga_write(" -> "); vga_write_dec((uint32_t)clen);
		vga_write(" bytes)\n");
	}
	kfree(in); kfree(out);
}

/* "gunzip <src> [dst]" — decompress a gzip file (from OwOS or host gzip). dst
 * defaults to <src> minus a trailing ".gz". */
static void cmd_gunzip(const char *arg)
{
	char src[24], dst[24];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0') {
		vga_write("usage: gunzip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		vga_write("no such file\n");
		return;
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	if (!in) {
		vga_write("gunzip: out of memory\n");
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int orig = (n < 0) ? -1 : gzip_orig_size(in, (uint32_t)n);
	if (orig < 0) {
		vga_write("gunzip: not a gzip file\n");
		kfree(in);
		return;
	}
	if (dst[0] == '\0') {                   /* strip a trailing ".gz"           */
		size_t l = str_len(src);
		if (l > 3 && src[l - 3] == '.' && src[l - 2] == 'g' &&
		    src[l - 1] == 'z') {
			str_copy(dst, src);
			dst[l - 3] = '\0';
		} else {
			vga_write("usage: gunzip <src> [dst]\n");
			kfree(in);
			return;
		}
	}

	uint8_t *out = kmalloc(orig ? (uint32_t)orig : 1);
	if (!out) {
		vga_write("gunzip: out of memory\n");
		kfree(in);
		return;
	}
	int dlen = gzip_decompress(in, (uint32_t)n, out, (uint32_t)orig);
	if (dlen < 0) {
		vga_write("gunzip: corrupt or unsupported gzip\n");
	} else if (fs_create(dst, out, (uint32_t)dlen) != 0) {
		vga_write("gunzip: write failed\n");
	} else {
		vga_write(src); vga_write(" -> "); vga_write(dst);
		vga_write(" ("); vga_write_dec((uint32_t)dlen);
		vga_write(" bytes)\n");
	}
	kfree(in); kfree(out);
}

#define REDIR_MAX (16 * 1024)  /* cap on captured output for "cmd > file" */

/* ---- built-in command table ---------------------------------------------
 * Adapters give every handler the uniform cmd_fn shape (one const char *arg).
 * No-arg commands ignore it; a few carry the small bit of glue that used to
 * sit inline in run()'s dispatch ladder. Handlers already taking a single
 * const char * go into the table directly. */
static void bi_clear(const char *a)    { (void)a; vga_clear(); }
static void bi_uptime(const char *a)
{
	(void)a;
	vga_write("up ");
	vga_write_dec(timer_seconds());
	vga_write(" seconds\n");
}
static void bi_sleep(const char *a)    { timer_sleep(parse_uint(a) * 1000); }
static void bi_meminfo(const char *a)  { (void)a; cmd_meminfo(); }
static void bi_memtest(const char *a)  { (void)a; cmd_memtest(); }
static void bi_tasks(const char *a)    { (void)a; sched_dump(); }
static void bi_ls(const char *a)
{
	if (a[0] == '\0') {
		fs_list(cwd);
		return;
	}
	char rpath[PATH_MAX];
	if (path_resolve(a, rpath) != 0)
		vga_write("ls: path too long\n");
	else
		fs_list(rpath);
}
static void bi_pwd(const char *a)      { (void)a; cmd_pwd(); }
static void bi_run(const char *a)      { cmd_run((char *)a); }
static void bi_format(const char *a)   { (void)a; fs_format(); vga_write("filesystem erased.\n"); }
static void bi_lspci(const char *a)    { (void)a; pci_list(); }
static void bi_lsusb(const char *a)
{
	while (*a == ' ') a++;            /* "lsusb <port>" shows detail for a port  */
	int port = -1;
	if (*a >= '0' && *a <= '9') {
		port = 0;
		while (*a >= '0' && *a <= '9')
			port = port * 10 + (*a++ - '0');
	}
	usb_list(port);
}
static void bi_storage(const char *a)  { (void)a; cmd_storage(); }

/* Safely detach the removable volume mounted at /media so the medium can be removed.
 * Writes are write-through, so there is nothing buffered to flush -- this just
 * drops the mount. `mount` re-probes a non-owofs disk and remounts. */
static void bi_eject(const char *a)
{
	(void)a;
	int hfs = hfs_mounted(), ext = ext_mounted();
	if (!fat_mounted() && !hfs && !ext) {
		vga_write("eject: nothing mounted at /media\n");
		return;
	}
	vga_write("eject: detaching ");
	vga_write(ext ? ext_type() : hfs ? hfs_type() : fat_type());
	const char *label = ext ? ext_label() : hfs ? hfs_label() : fat_label();
	if (label[0]) {
		vga_write(" \"");
		vga_write(label);
		vga_write("\"");
	}
	vga_write(" from /media -- safe to remove\n");
	if (ext)      ext_unmount();
	else if (hfs) hfs_unmount();
	else          fat_unmount();
}

static void bi_mount(const char *a)
{
	(void)a;
	if (fat_mounted() || hfs_mounted() || ext_mounted()) {
		vga_write("mount: a volume is already mounted at /media\n");
		return;
	}
	if (blk_mount_fat() != 0)
		vga_write("mount: no FAT/HFS+/ext volume found\n");
}
static void bi_beep(const char *a)     { (void)a; speaker_beep(1000, 200); }
static void bi_play(const char *a)
{
	if (a[0] == '\0')                       /* bare "play" -> the default tune  */
		speaker_song();
	else if (fs_size(a) >= 0)               /* a file -> treat it as a WAV      */
		sb16_play_wav(a);
	else if (speaker_play(a) != 0) {
		vga_write("play: unknown tune: ");
		vga_write(a);
		vga_write("\ntunes: twinkle, megalovania, sweden (or a .wav file)\n");
	}
}
static void bi_cpuinfo(const char *a)  { (void)a; cmd_cpuinfo(); }
static void bi_whoami(const char *a)   { (void)a; cmd_whoami(); }
static void bi_which(const char *a)    { cmd_which((char *)a); }
static void bi_date(const char *a)     { (void)a; cmd_date(); }
static void bi_neofetch(const char *a) { (void)a; neofetch(); }
static void bi_ping(const char *a)
{
	if (a[0] == '\0') { vga_write("usage: ping <host>\n"); return; }
	net_ping(a);
}
static void bi_dig(const char *a)
{
	if (a[0] == '\0') { vga_write("usage: dig <host> [type]\n"); return; }
	net_dig(a);
}
static void bi_fstest(const char *a)     { (void)a; fs_selftest(); }
static void bi_cryptotest(const char *a) { (void)a; crypto_selftest(); }
static void bi_echo(const char *a)       { vga_write(a); vga_putc('\n'); }
static void bi_poweroff(const char *a)
{
	(void)a;
	vga_write("powering off...\n");
	power_off();
	vga_write("poweroff not supported here\n");
}
static void bi_restart(const char *a)    { (void)a; vga_write("restarting...\n"); power_reset(); }

/* The one and only command list. Add a built-in here — that wires it into
 * dispatch, `which`, and `help` at once. Order is the order `help` prints.
 * fn == NULL rows are help-only notes for things that aren't bare commands. */
static const struct builtin builtins[] = {
	{ "help",     cmd_help,     "help          show this list" },
	{ "clear",    bi_clear,     "clear         clear the screen" },
	{ "uptime",   bi_uptime,    "uptime        seconds since boot" },
	{ "sleep",    bi_sleep,     "sleep <secs>  block for a number of seconds" },
	{ "meminfo",  bi_meminfo,   "meminfo       physical and heap memory usage" },
	{ "memtest",  bi_memtest,   "memtest       allocate/free check of the heap" },
	{ "tasks",    bi_tasks,     "tasks         list scheduler threads and run counts" },
	{ "ls",       bi_ls,        "ls [path]     list a directory (default: current)" },
	{ "pwd",      bi_pwd,       "pwd           print the working directory" },
	{ "cd",       cmd_cd,       "cd [dir]      change directory (no arg -> home)" },
	{ "cat",      cmd_cat,      "cat <file>    print a file" },
	{ "grep",     cmd_grep,     "grep <p> [f]  lines containing <p>, from <f> or a pipe\n"
	                            "  <cmd> | grep <p>  filter any command's output" },
	{ "write",    cmd_write,    "write <f> <t> write text to a file" },
	{ "cp",       cmd_cp,       "cp <src> <dst> copy a file" },
	{ "mv",       cmd_mv,       "mv <src> <dst> rename/move a file or directory" },
	{ "rm",       cmd_rm,       "rm <file>...  delete files" },
	{ "mkdir",    cmd_mkdir,    "mkdir <dir>...  create directories" },
	{ "rmdir",    cmd_rmdir,    "rmdir <dir>...  remove empty directories" },
	{ "zip",      cmd_zip,      "zip <s> [d]   compress a file (d defaults to <s>.z)" },
	{ "unzip",    cmd_unzip,    "unzip <s> [d] decompress a file (d defaults to <s> minus .z)" },
	{ "gzip",     cmd_gzip,     "gzip <s> [d]  compress to gzip (d defaults to <s>.gz)" },
	{ "gunzip",   cmd_gunzip,   "gunzip <s> [d] decompress a gzip file (d defaults to <s> minus .gz)" },
	{ "run",      bi_run,       "run <f> [a|b] run a program (ring 3); a|b pipes a into b" },
	{ "format",   bi_format,    "format        erase the filesystem" },
	{ "lspci",    bi_lspci,     "lspci         list PCI devices" },
	{ "lsusb",    bi_lsusb,     "lsusb [port]  list USB devices (port: detail for one)" },
	{ "storage",  bi_storage,   "storage       list storage devices (IDE/SATA/NVMe)" },
	{ "mount",    bi_mount,     "mount         mount a removable volume (USB/SD/disk) at /media" },
	{ "eject",    bi_eject,     "eject         safely detach the removable volume at /media" },
	{ "beep",     bi_beep,      "beep          beep the PC speaker" },
	{ "play",     bi_play,      "play [arg]    play a tune (twinkle/megalovania/sweden) or a .wav file" },
	{ "cpuinfo",  bi_cpuinfo,   "cpuinfo       CPU vendor, model, and feature flags" },
	{ "uname",    cmd_uname,    "uname [-a]    system identity" },
	{ "whoami",   bi_whoami,    "whoami        current user" },
	{ "which",    bi_which,     "which <name>  locate a command (built-in or file)" },
	{ "loadkeys", cmd_loadkeys, "loadkeys <l>  set keyboard layout (us, fr)" },
	{ "date",     bi_date,      "date          current date and time" },
	{ "neofetch", bi_neofetch,  "neofetch      system info splash" },
	{ "ping",     bi_ping,      "ping <host>   ICMP-echo a host or IP (e.g. google.com)" },
	{ "dig",      bi_dig,       "dig <host> [t] DNS lookup; type t=A,AAAA,CNAME,MX,TXT,NS..." },
	{ NULL,       NULL,         "curl <url>    HTTP an http:// or https:// URL (-i headers, -X method, -o file)" },
	{ "url",      cmd_url,      "url <url>     parse a URL into its parts and rebuild it" },
	{ "torrent",  cmd_torrent,  "torrent info <f> parse a .torrent: name, infohash, files (test: self-check)\n"
	                            "  torrent peers <f> announce to the tracker and list returned peers\n"
	                            "  torrent peer <f> <ip> <port> leech piece 0 from a peer, verify sha1\n"
	                            "  torrent get <f>  download a whole single-file torrent to disk" },
	{ "services", services_cmd, "services [..] manage headless daemons (list/start/stop/restart/status)" },
	{ "echo",     bi_echo,      "echo <text>   print text back" },
	{ NULL,       NULL,         "<cmd> > <f>   redirect a command's output into a file" },
	{ "poweroff", bi_poweroff,  "poweroff      shut the machine down" },
	{ "restart",  bi_restart,   "restart       reboot the machine" },
	/* dispatchable but intentionally absent from `help` (developer self-tests) */
	{ "fstest",     bi_fstest,     NULL },
	{ "cryptotest", bi_cryptotest, NULL },
	{ NULL, NULL, NULL },   /* sentinel */
};

/* Resolve a command name to its row, skipping help-only (fn == NULL) entries. */
static const struct builtin *builtin_find(const char *name)
{
	for (const struct builtin *b = builtins; b->name || b->help; b++)
		if (b->fn && streq(b->name, name))
			return b;
	return NULL;
}

static int is_builtin(const char *name)
{
	return builtin_find(name) != NULL;
}

static void cmd_help(const char *a)
{
	(void)a;
	vga_write("commands:\n");
	for (const struct builtin *b = builtins; b->name || b->help; b++) {
		if (!b->help)
			continue;
		vga_write("  ");
		vga_write(b->help);
		vga_putc('\n');
	}
}

static void run(char *cmd)
{
	if (cmd[0] == '\0')
		return;                /* blank line                              */

	/* "<cmd> > <file>": capture cmd's text output and write it to file.
	 * Works for any built-in because all of them emit through vga_putc. */
	char *gt = cmd;
	while (*gt && *gt != '>')
		gt++;
	if (*gt == '>') {
		*gt = '\0';
		char *fname = gt + 1;
		while (*fname == ' ')                  /* skip blanks after '>'    */
			fname++;
		char *fend = fname;                    /* stop at first blank      */
		while (*fend && *fend != ' ')
			fend++;
		*fend = '\0';
		char *cend = gt;                       /* trim blanks before '>'   */
		while (cend > cmd && cend[-1] == ' ')
			*--cend = '\0';

		if (cmd[0] == '\0' || fname[0] == '\0') {
			vga_write("usage: <command> > <file>\n");
			return;
		}
		char *cap = kmalloc(REDIR_MAX);
		if (!cap) {
			vga_write("redirect: out of memory\n");
			return;
		}
		vga_capture_begin(cap, REDIR_MAX);
		run(cmd);                              /* no '>' left: dispatches  */
		uint32_t n = vga_capture_end();
		if (fs_create(fname, cap, n) == 0) {
			vga_write("wrote ");
			vga_write_dec(n);
			vga_write(" bytes to ");
			vga_write(fname);
			vga_putc('\n');
		} else {
			vga_write("redirect: write failed\n");
		}
		kfree(cap);
		return;
	}

	/* Generic pipe "A | B": capture A's console output and feed it to B as
	 * piped input, which input-consuming builtins (grep) read. Each stage's
	 * capture ends before the next runs, so pipes chain (A | grep x | grep y).
	 * Program pipelines ("run a | b") have their own concurrent path inside
	 * cmd_run, so those are left to it. */
	if (after_prefix(cmd, "run ") == NULL) {
		char *bar = cmd;
		while (*bar && *bar != '|')
			bar++;
		if (*bar == '|') {
			*bar = '\0';
			char *left = cmd, *right = bar + 1;
			while (*right == ' ')
				right++;
			char *lend = bar;              /* trim blanks before '|'   */
			while (lend > left && lend[-1] == ' ')
				*--lend = '\0';
			if (left[0] == '\0' || right[0] == '\0') {
				vga_write("usage: <command> | grep <pattern>\n");
				return;
			}
			char *cap = kmalloc(REDIR_MAX);
			if (!cap) {
				vga_write("pipe: out of memory\n");
				return;
			}
			vga_capture_begin(cap, REDIR_MAX);
			run(left);
			uint32_t n = vga_capture_end();
			if (n >= REDIR_MAX)
				n = REDIR_MAX - 1;
			cap[n] = '\0';
			g_pipe_in = cap;
			run(right);
			g_pipe_in = NULL;
			kfree(cap);
			return;
		}
	}

	/* Split off the first whitespace-delimited token (the command name); the
	 * rest, leading blanks trimmed, is its argument string ("" when none).
	 * `cmd` is left intact so the /bin fallback can re-tokenize the whole line. */
	char name[64];
	int n = 0;
	while (cmd[n] && cmd[n] != ' ' && n < (int)sizeof(name) - 1) {
		name[n] = cmd[n];
		n++;
	}
	name[n] = '\0';
	const char *carg = cmd + n;
	while (*carg == ' ')
		carg++;

	const struct builtin *b = builtin_find(name);
	if (b) {
		b->fn(carg);
		return;
	}

	/* Not a builtin: treat it like a shell would — look for a program of that
	 * name (cwd, then /bin) and run it in ring 3 with its arguments. This is
	 * how every user program (hello, args, curl, ...) launches by bare name;
	 * none of them is wired into the command table above. */
	char *av[8] = {0};
	int ac = tokenize(cmd, av, 8);
	int pid = (ac > 0) ? spawn(ac, av) : -1;
	if (pid >= 0) {
		wait_reap(pid);
	} else if (ac > 0) {
		vga_write("unknown command: ");
		vga_write(av[0]);
		vga_write("\ntype 'help'\n");
	}
}

void shell_feed(char c)
{
	switch (c) {
	case '\n':
		vga_set_pos((uint16_t)(edit_start + line_len));  /* end of line     */
		vga_putc('\n');
		line[line_len] = '\0';
		hist_add(line);
		run(line);
		prompt();
		return;
	case '\b':                             /* backspace: delete left of cursor */
		if (cursor > 0) {
			int at_end = (cursor == line_len);
			for (size_t i = cursor - 1; i + 1 < line_len; i++)
				line[i] = line[i + 1];
			cursor--;
			line_len--;
			if (at_end) {              /* fast path: just erase the last char */
				vga_putc('\b');
				render_len = line_len;
			} else {
				edit_redraw();
			}
		}
		return;
	case KEY_DELETE:                       /* delete char under the cursor     */
		if (cursor < line_len) {
			for (size_t i = cursor; i + 1 < line_len; i++)
				line[i] = line[i + 1];
			line_len--;
			edit_redraw();
		}
		return;
	case KEY_LEFT:
		if (cursor > 0)
			vga_set_pos((uint16_t)(edit_start + --cursor));
		return;
	case KEY_RIGHT:
		if (cursor < line_len)
			vga_set_pos((uint16_t)(edit_start + ++cursor));
		return;
	case KEY_HOME:
		cursor = 0;
		vga_set_pos(edit_start);
		return;
	case KEY_END:
		cursor = line_len;
		vga_set_pos((uint16_t)(edit_start + line_len));
		return;
	case KEY_UP:
		hist_up();
		return;
	case KEY_DOWN:
		hist_down();
		return;
	case KEY_KILL:                         /* Ctrl+U: erase the whole line     */
		line_len = cursor = 0;
		edit_redraw();
		return;
	case KEY_CLEAR:                        /* Ctrl+L: clear, keep the line     */
		vga_clear();
		print_prompt();
		edit_start = vga_get_pos();
		render_len = 0;
		edit_redraw();
		return;
	default:
		/* Insert a printable byte at the cursor. CP437 high bytes (>= 0x80,
		 * negative as signed char) are accent characters from the FR layout;
		 * 0x15 is its CP437 '§'. Other control codes are ignored. */
		if (((unsigned char)c >= 0x20 || (unsigned char)c == 0x15) &&
		    line_len < LINE_MAX - 1) {
			int at_end = (cursor == line_len);
			for (size_t i = line_len; i > cursor; i--)
				line[i] = line[i - 1];
			line[cursor] = c;
			cursor++;
			line_len++;
			if (at_end) {              /* fast path: just echo the new char  */
				uint32_t s0 = vga_scroll_count();
				vga_putc(c);
				render_len = line_len;
				edit_fix_anchor(s0);   /* the echo may have scrolled         */
			} else {
				edit_redraw();
			}
		}
		return;
	}
}

/* Ctrl+C at the prompt: abandon whatever is on the line and start fresh. (A
 * Ctrl+C that lands while a command is running is handled by the command
 * itself; by the time control is back here the line is already empty.) */
void shell_interrupt(void)
{
	vga_set_pos((uint16_t)(edit_start + line_len));
	vga_write("^C\n");
	prompt();
}

void shell_init(void)
{
	vga_write("\ntype 'help' for a list of commands.\n");
	prompt();
}

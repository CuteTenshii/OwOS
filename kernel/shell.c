/* shell.c — line editing and a handful of built-in commands. */

#include <stddef.h>
#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "kstring.h"
#include "log.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "fs.h"
#include "fat.h"
#include "hfs.h"
#include "ext.h"
#include "iso9660.h"
#include "blk.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "pci.h"
#include "e1000.h"
#include "usb.h"
#include "xhci.h"
#include "sb16.h"
#include "hda.h"
#include "cpu.h"
#include "rtc.h"
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
	uint32_t rows = con_scroll_count() - before;
	if (rows)
		edit_start = con_pos_scroll_up(edit_start, rows);
}

/* Redraw the current line in place: text from edit_start, spaces to wipe any
 * leftover tail from a previously longer line, then park the cursor at the
 * insertion point. Stays correct even if the line is long enough to scroll the
 * screen mid-redraw -- edit_start is re-anchored against any scrolling. */
static void edit_redraw(void)
{
	uint32_t s0 = con_scroll_count();
	con_set_pos(edit_start);
	for (size_t i = 0; i < line_len; i++)
		con_putc(line[i]);
	for (size_t i = line_len; i < render_len; i++)
		con_putc(' ');
	render_len = line_len;
	edit_fix_anchor(s0);
	con_set_pos((uint16_t)(edit_start + cursor));
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
	con_set_color(CON_LIGHT_CYAN, CON_BLACK);
	con_write("owos:");
	/* Show the working directory, abbreviating the home directory to "~". */
	size_t hlen = strlen(HOME_DIR);
	int in_home = 1;
	for (size_t i = 0; i < hlen; i++)
		if (cwd[i] != HOME_DIR[i]) { in_home = 0; break; }
	if (in_home && (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
		con_putc('~');
		con_write(cwd + hlen);                 /* the path below home, if any */
	} else {
		con_write(cwd);
	}
	con_write("$ ");
	con_set_color(CON_LIGHT_GREY, CON_BLACK);
}

/* Begin a fresh input line: draw the prompt and reset the editor state. */
static void prompt(void)
{
	print_prompt();
	edit_start = con_get_pos();
	line_len = cursor = render_len = 0;
}

/* "help" — list every built-in. The text is data in the `builtins` table just
 * above run(), so this walks that table; the definition lives there too. */
static void cmd_help(const char *a);

static void print_kib(const char *label, uint32_t kib)
{
	con_write(label);
	con_write_dec(kib);
	con_write(" KiB\n");
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
		con_write_dec(tenths / 10);
		con_putc('.');
		con_write_dec(tenths % 10);
		con_write(" GiB");
	} else {
		con_write_dec(mib);
		con_write(" MiB");
	}
}

static void cmd_storage(void)
{
	uint8_t buf[512];
	int n = 0;

	if (ata_present()) {
		con_write("  IDE/ATA    ");
		con_write(ata_model());
		con_write("  ");
		write_capacity(ata_sectors());
		con_write(" (PIO, primary master)\n");
		n++;
	}
	if (ahci_present()) {
		con_write("  SATA/AHCI  ");
		con_write(ahci_model());
		con_write("  ");
		write_capacity(ahci_sectors());
		con_write(" (DMA) -- read LBA0: ");
		con_write(ahci_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (nvme_present()) {
		con_write("  NVMe       ");
		con_write(nvme_model());
		con_write("  ");
		write_capacity(nvme_sectors());
		con_write(" (ns 1) -- read LBA0: ");
		con_write(nvme_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (xhci_msc_present()) {
		con_write("  USB        ");
		con_write(xhci_msc_model());
		con_write("  ");
		write_capacity(xhci_msc_sectors());
		con_write(" (xHCI BOT) -- read LBA0: ");
		/* Safe even on a flaky adapter: every wait in the read is bounded
		 * (event_wait times out in 500 ms) and sti-safe (timer_sleep does
		 * sti;hlt), and a controller that goes silent drops the device
		 * (hc_responsive -> msc_ready=0). So this reports FAIL, never a freeze. */
		con_write(xhci_msc_read(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (!n)
		con_write("  no storage devices detected\n");
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
		con_write("memtest: FAIL (bad allocation)\n");
		return;
	}
	for (int i = 0; i < 64; i++) {
		a[i] = (char)('A' + (i % 26));
		b[i] = (char)('a' + (i % 26));
	}
	if (a[0] != 'A' || a[25] != 'Z' || b[0] != 'a') {
		con_write("memtest: FAIL (data corrupted)\n");
		return;
	}
	kfree(a);
	char *c = kmalloc(64);          /* should reclaim a's slot                */
	if (c != a) {
		con_write("memtest: FAIL (no reuse after free)\n");
		return;
	}
	kfree(b);
	kfree(c);
	con_write("memtest: PASS\n");
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

	/* Resolve the program to a path: cwd-relative (or absolute), then /bin. */
	char path[PATH_MAX];
	int sz = -1;
	if (path_resolve(argv[0], path) == 0)
		sz = fs_size(path);
	if (sz < 0 && argv[0][0] != '/') {             /* fall back to /bin/<name>  */
		int j = 0;
		const char *pfx = "/bin/";
		for (int i = 0; pfx[i]; i++)
			path[j++] = pfx[i];
		for (int i = 0; argv[0][i] && j < PATH_MAX - 1; i++)
			path[j++] = argv[0][i];
		path[j] = '\0';
		sz = fs_size(path);
	}
	if (sz < 0)
		return -1;

	/* Size the load buffer to the file -- programs are 120+ KiB now, so a fixed
	 * small buffer would truncate the ELF and the load would fail. */
	uint8_t *buf = kmalloc((uint32_t)sz);
	if (!buf)
		return -1;
	int n = fs_read(path, buf, (uint32_t)sz);
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
	con_write("[pid ");
	con_write_dec((uint32_t)pid);
	con_write(" exited, status ");
	con_write_dec((uint32_t)status);
	con_write("]\n");
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
			con_write("usage: run <a> | <b>\n");
			return;
		}
		int id = pipe_alloc();
		if (id < 0) {
			con_write("out of pipes\n");
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
			con_write("pipeline: bad program\n");
			return;
		}
		wait_reap(p1);
		wait_reap(p2);
		return;
	}

	char *argv[8];
	int argc = tokenize(args, argv, 8);
	if (argc == 0) {
		con_write("usage: run <file> [args]\n");
		return;
	}
	int pid = spawn(argc, argv);
	if (pid < 0) {
		con_write("no such file / not a valid program\n");
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
			con_write(line_start);
			con_putc('\n');
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
		con_write("usage: grep <pattern> <file>  (or: <cmd> | grep <pattern>)\n");
		return;
	}

	if (name[0] != '\0') {                  /* file mode                      */
		char *buf = kmalloc(4096);
		if (!buf)
			return;
		int n = fs_read(name, buf, 4095);
		if (n < 0) {
			con_write("no such file\n");
		} else {
			buf[n] = '\0';
			grep_emit(pattern, buf);
		}
		kfree(buf);
	} else if (g_pipe_in) {                 /* piped mode                     */
		grep_emit(pattern, g_pipe_in);
	} else {
		con_write("usage: grep <pattern> <file>  (or: <cmd> | grep <pattern>)\n");
	}
}

static void cmd_cat(const char *name)
{
	char rpath[PATH_MAX];
	if (path_resolve(name, rpath) != 0) {
		con_write("cat: path too long\n");
		return;
	}
	char *buf = kmalloc(4096);
	if (!buf)
		return;
	int n = fs_read(rpath, buf, 4095);
	if (n < 0) {
		con_write("no such file\n");
	} else {
		buf[n] = '\0';
		con_write(buf);
		if (n > 0 && buf[n - 1] != '\n')
			con_putc('\n');
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
		con_write("no such file\n");
		return NULL;
	}
	uint8_t *buf = kmalloc(sz ? (uint32_t)sz : 1);
	if (!buf) {
		con_write("torrent: out of memory\n");
		return NULL;
	}
	int n = fs_read(file, buf, (uint32_t)sz);
	if (n < 0 || torrent_parse(t, buf, (uint32_t)n) != 0) {
		con_write("torrent: not a valid .torrent file\n");
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
		con_write("usage: torrent peer <file> <ip> <port>\n");
		return;
	}
	while (*after == ' ') after++;
	uint16_t port = (uint16_t)parse_uint(after);
	if (port == 0) {
		con_write("usage: torrent peer <file> <ip> <port>\n");
		return;
	}

	struct torrent t;
	uint8_t *buf = load_torrent(file, &t);
	if (!buf)
		return;

	uint8_t *piece = kmalloc(t.piece_length ? (uint32_t)t.piece_length : 1);
	if (!piece) {
		con_write("torrent: out of memory\n");
		torrent_free(&t);
		kfree(buf);
		return;
	}
	int n = peer_fetch_piece(&t, ip, port, 0, piece);
	if (n > 0) {
		con_write("got piece 0: ");
		con_write_dec((uint32_t)n);
		con_write(" bytes\n");
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
		con_write("get: multi-file torrents not supported yet\n");
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
		con_write("get: no peers available\n");
		torrent_free(&t);
		kfree(buf);
		return;
	}

	if (fs_prealloc(outname, (uint32_t)t.total_length) != 0) {
		con_write("get: cannot allocate output file\n");
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
		con_write("get: out of memory\n");
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
		con_write("get: could not connect to any peer\n");

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
				con_putc('.');
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
		con_putc('\n');
	for (int i = 0; i < nconn; i++) {
		if (pc[i]) peer_disconnect(pc[i]);
		kfree(pbuf[i]);
	}
	kfree(done_f); kfree(inflt);

	if (disk_fail)
		con_write("get: disk write failed\n");

	if (done == t.n_pieces) {
		con_write("get: complete -- ");
		con_write_dec((uint32_t)t.total_length);
		con_write(" bytes verified and saved to ");
		con_write(outname);
		con_putc('\n');
	} else {
		con_write("get: incomplete (");
		con_write_dec(done);
		con_write("/");
		con_write_dec(t.n_pieces);
		con_write(" pieces)\n");
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
		con_write("usage: torrent info <file> | torrent peers <file> "
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
	con_putc('0' + (char)((v / 10) % 10));
	con_putc('0' + (char)(v % 10));
}

/* url <string> — parse a URL into its parts and rebuild it (parser self-demo). */
static void cmd_url(const char *arg)
{
	struct url u;
	if (!url_parse(arg, &u)) {
		con_write("url: malformed URL\n");
		return;
	}
	con_write("  scheme:   "); con_write(u.scheme[0] ? u.scheme : "(none)"); con_putc('\n');
	if (u.userinfo[0]) { con_write("  userinfo: "); con_write(u.userinfo); con_putc('\n'); }
	con_write("  host:     "); con_write(u.host); con_putc('\n');
	con_write("  port:     "); con_write_dec(u.port);
	con_write(u.has_port ? "\n" : " (default)\n");
	con_write("  path:     "); con_write(u.path[0] ? u.path : "(empty)"); con_putc('\n');
	if (u.has_query)    { con_write("  query:    "); con_write(u.query); con_putc('\n'); }
	if (u.has_fragment) { con_write("  fragment: "); con_write(u.fragment); con_putc('\n'); }

	char rebuilt[640];
	if (url_build(&u, rebuilt, sizeof(rebuilt)) >= 0) {
		con_write("  rebuilt:  "); con_write(rebuilt); con_putc('\n');
	}
}

static void cmd_date(void)
{
	struct rtc_time t;
	rtc_now(&t);
	con_write_dec(t.year); con_putc('-'); wr2(t.month); con_putc('-'); wr2(t.day);
	con_putc(' ');
	wr2(t.hour); con_putc(':'); wr2(t.min); con_putc(':'); wr2(t.sec);
	con_putc('\n');
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

	con_write("vendor: "); con_write(vendor); con_putc('\n');
	con_write("model:  "); con_write(brand);  con_putc('\n');
	con_write("family: ");  con_write_dec(sig.family);
	con_write("  model: "); con_write_dec(sig.model);
	con_write("  stepping: "); con_write_dec(sig.stepping); con_putc('\n');
	con_write("address: "); con_write_dec(phys);
	con_write(" bits phys, "); con_write_dec(virt); con_write(" bits virt\n");
	uint32_t mhz = cpu_mhz();
	if (mhz) {
		con_write("clock:  ~"); con_write_dec(mhz); con_write(" MHz\n");
	}
	con_write("flags:  "); con_write(flags); con_putc('\n');
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
		con_write(UNAME_SYSNAME);  con_putc(' ');
		con_write(UNAME_NODENAME); con_putc(' ');
		con_write(UNAME_RELEASE);  con_putc(' ');
		con_write(UNAME_MACHINE);  con_putc('\n');
		break;
	case 's':
	case 'o': con_write(UNAME_SYSNAME);  con_putc('\n'); break;
	case 'n': con_write(UNAME_NODENAME); con_putc('\n'); break;
	case 'r': con_write(UNAME_RELEASE);  con_putc('\n'); break;
	case 'm': con_write(UNAME_MACHINE);  con_putc('\n'); break;
	default:
		con_write("usage: uname [-asnrmo]\n");
		break;
	}
}

/* "whoami" — OwOS is single-user, so the shell always runs as root. */
static void cmd_whoami(void)
{
	con_write("root\n");
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
		con_write("usage: which <name>...\n");
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
			con_write(name);
			con_write(": shell built-in\n");
		} else if (path_resolve(name, rpath) == 0 && fs_size(rpath) >= 0) {
			con_write(name); con_write(": "); con_write(rpath);
			con_write(" (file)\n");
		} else if (name[0] != '/' && fs_size(binp) >= 0) {
			con_write(name); con_write(": "); con_write(binp);
			con_write(" (file)\n");
		} else {
			con_write("which: no ");
			con_write(name);
			con_write(" in (built-ins, owofs)\n");
		}
	}
}

/* "loadkeys <layout>" — switch the keyboard layout (us, fr). */
static void cmd_loadkeys(const char *arg)
{
	while (*arg == ' ')
		arg++;
	if (arg[0] == '\0') {
		con_write("usage: loadkeys <us|fr>\n");
		return;
	}
	if (keyboard_set_layout(arg) == 0) {
		con_write("keyboard layout: ");
		con_write(arg);
		con_putc('\n');
	} else {
		con_write("loadkeys: unknown layout (try us or fr)\n");
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
		con_write("usage: write <file> <text>\n");
		return;
	}
	char rpath[PATH_MAX];
	if (path_resolve(name, rpath) != 0) {
		con_write("write: path too long\n");
		return;
	}
	if (fs_create(rpath, text, (uint32_t)strlen(text)) == 0) {
		con_write("wrote ");
		con_write(rpath);
		con_putc('\n');
	} else {
		con_write("write failed\n");
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
	int n = (int)strlen(path);
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
		con_write("cd: path too long\n");
		return;
	}
	int d = fs_isdir(target);
	if (d < 0) {
		con_write("cd: no such directory: "); con_write(a); con_putc('\n');
		return;
	}
	if (d == 0) {
		con_write("cd: not a directory: "); con_write(a); con_putc('\n');
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
	con_write(cwd);
	con_putc('\n');
}

/* "cp <src> <dst>" — copy a file's contents into a new (or overwritten) file.
 * If <dst> is a directory, the copy lands inside it under <src>'s basename. */
static void cmd_cp(const char *arg)
{
	char src[PATH_MAX], dst[PATH_MAX], rsrc[PATH_MAX], rdst[PATH_MAX];
	parse_two(arg, src, dst, (int)sizeof(src));
	if (src[0] == '\0' || dst[0] == '\0') {
		con_write("usage: cp <src> <dst>\n");
		return;
	}
	if (path_resolve(src, rsrc) != 0 || path_resolve(dst, rdst) != 0) {
		con_write("cp: path too long\n");
		return;
	}
	into_dir(rdst, rsrc);
	if (streq(rsrc, rdst)) {
		con_write("cp: source and destination are the same\n");
		return;
	}
	int sz = fs_size(rsrc);
	if (sz < 0) {
		con_write("cp: no such file: "); con_write(src); con_putc('\n');
		return;
	}
	uint8_t *buf = kmalloc(sz ? (uint32_t)sz : 1);
	if (!buf) {
		con_write("cp: out of memory\n");
		return;
	}
	int n = fs_read(rsrc, buf, (uint32_t)sz);
	if (n < 0)
		con_write("cp: read failed\n");
	else if (fs_create(rdst, buf, (uint32_t)n) != 0)
		con_write("cp: write failed (disk or directory full?)\n");
	else {
		con_write(rsrc); con_write(" -> "); con_write(rdst); con_putc('\n');
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
		con_write("usage: mv <src> <dst>\n");
		return;
	}
	if (path_resolve(src, rsrc) != 0 || path_resolve(dst, rdst) != 0) {
		con_write("mv: path too long\n");
		return;
	}
	into_dir(rdst, rsrc);
	if (streq(rsrc, rdst))
		return;                                 /* a no-op, like real mv            */
	if (fs_rename(rsrc, rdst) != 0) {
		con_write("mv: failed (no such source, or illegal move)\n");
		return;
	}
	con_write(rsrc); con_write(" -> "); con_write(rdst); con_putc('\n');
}

/* "rm <file> [file...]" — delete one or more files (sectors leak). */
static void cmd_rm(const char *arg)
{
	char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
	parse_two(arg, name, extra, (int)sizeof(name));
	if (name[0] == '\0') {
		con_write("usage: rm <file>\n");
		return;
	}
	if (path_resolve(name, rpath) != 0) {
		con_write("rm: path too long\n");
	} else {
		int r = fs_delete(rpath);
		if (r == 0) {
			con_write("removed "); con_write(rpath); con_putc('\n');
		} else if (r == -2) {
			con_write("rm: is a directory (use rmdir): ");
			con_write(name); con_putc('\n');
		} else {
			con_write("rm: no such file: "); con_write(name); con_putc('\n');
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
		con_write("usage: mkdir <dir>\n");
		return;
	}
	if (path_resolve(name, rpath) != 0)
		con_write("mkdir: path too long\n");
	else if (fs_mkdir(rpath) == 0) {
		con_write("created "); con_write(rpath); con_putc('\n');
	} else
		con_write("mkdir: cannot create (exists, bad path, or directory full)\n");
	if (extra[0])
		cmd_mkdir(extra);
}

/* "rmdir <dir>" — remove an empty directory. */
static void cmd_rmdir(const char *arg)
{
	char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
	parse_two(arg, name, extra, (int)sizeof(name));
	if (name[0] == '\0') {
		con_write("usage: rmdir <dir>\n");
		return;
	}
	if (path_resolve(name, rpath) != 0) {
		con_write("rmdir: path too long\n");
	} else {
		int r = fs_rmdir(rpath);
		if (r == 0) {
			con_write("removed "); con_write(rpath); con_putc('\n');
		} else if (r == -2) {
			con_write("rmdir: not a directory: "); con_write(name); con_putc('\n');
		} else if (r == -3) {
			con_write("rmdir: directory not empty: "); con_write(name); con_putc('\n');
		} else {
			con_write("rmdir: no such directory: "); con_write(name); con_putc('\n');
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
		con_write("usage: zip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		con_write("no such file\n");
		return;
	}
	if (dst[0] == '\0') {                   /* derive <src>.z                   */
		size_t n = strlen(src);
		if (n + 2 >= sizeof(dst)) {
			con_write("zip: name too long, pass an explicit dst\n");
			return;
		}
		str_copy(dst, src);
		dst[n] = '.'; dst[n + 1] = 'z'; dst[n + 2] = '\0';
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	uint8_t *out = kmalloc(zip_bound((uint32_t)sz));
	if (!in || !out) {
		con_write("zip: out of memory\n");
		kfree(in); kfree(out);
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int clen = (n < 0) ? -1 :
		zip_compress(in, (uint32_t)n, out, zip_bound((uint32_t)sz));
	if (clen < 0) {
		con_write("zip: compression failed\n");
	} else if (fs_create(dst, out, (uint32_t)clen) != 0) {
		con_write("zip: write failed\n");
	} else {
		con_write(src); con_write(" -> "); con_write(dst);
		con_write(" ("); con_write_dec((uint32_t)n);
		con_write(" -> "); con_write_dec((uint32_t)clen);
		con_write(" bytes)\n");
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
		con_write("usage: unzip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		con_write("no such file\n");
		return;
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	if (!in) {
		con_write("unzip: out of memory\n");
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int orig = (n < 0) ? -1 : zip_orig_size(in, (uint32_t)n);
	if (orig < 0) {
		con_write("unzip: not an OwOS zip file\n");
		kfree(in);
		return;
	}
	if (dst[0] == '\0') {                   /* strip a trailing ".z"            */
		size_t l = strlen(src);
		if (l > 2 && src[l - 2] == '.' && src[l - 1] == 'z') {
			str_copy(dst, src);
			dst[l - 2] = '\0';
		} else {
			con_write("usage: unzip <src> [dst]\n");
			kfree(in);
			return;
		}
	}

	uint8_t *out = kmalloc(orig ? (uint32_t)orig : 1);
	if (!out) {
		con_write("unzip: out of memory\n");
		kfree(in);
		return;
	}
	int dlen = zip_decompress(in, (uint32_t)n, out, (uint32_t)orig);
	if (dlen < 0) {
		con_write("unzip: corrupt archive\n");
	} else if (fs_create(dst, out, (uint32_t)dlen) != 0) {
		con_write("unzip: write failed\n");
	} else {
		con_write(src); con_write(" -> "); con_write(dst);
		con_write(" ("); con_write_dec((uint32_t)dlen);
		con_write(" bytes)\n");
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
		con_write("usage: gzip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		con_write("no such file\n");
		return;
	}
	if (dst[0] == '\0') {                   /* derive <src>.gz                  */
		size_t n = strlen(src);
		if (n + 3 >= sizeof(dst)) {
			con_write("gzip: name too long, pass an explicit dst\n");
			return;
		}
		str_copy(dst, src);
		dst[n] = '.'; dst[n + 1] = 'g'; dst[n + 2] = 'z'; dst[n + 3] = '\0';
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	uint8_t *out = kmalloc(gzip_bound((uint32_t)sz));
	if (!in || !out) {
		con_write("gzip: out of memory\n");
		kfree(in); kfree(out);
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int clen = (n < 0) ? -1 :
		gzip_compress(in, (uint32_t)n, out, gzip_bound((uint32_t)sz));
	if (clen < 0) {
		con_write("gzip: compression failed\n");
	} else if (fs_create(dst, out, (uint32_t)clen) != 0) {
		con_write("gzip: write failed\n");
	} else {
		con_write(src); con_write(" -> "); con_write(dst);
		con_write(" ("); con_write_dec((uint32_t)n);
		con_write(" -> "); con_write_dec((uint32_t)clen);
		con_write(" bytes)\n");
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
		con_write("usage: gunzip <src> [dst]\n");
		return;
	}
	int sz = fs_size(src);
	if (sz < 0) {
		con_write("no such file\n");
		return;
	}

	uint8_t *in = kmalloc(sz ? (uint32_t)sz : 1);
	if (!in) {
		con_write("gunzip: out of memory\n");
		return;
	}
	int n = fs_read(src, in, (uint32_t)sz);
	int orig = (n < 0) ? -1 : gzip_orig_size(in, (uint32_t)n);
	if (orig < 0) {
		con_write("gunzip: not a gzip file\n");
		kfree(in);
		return;
	}
	if (dst[0] == '\0') {                   /* strip a trailing ".gz"           */
		size_t l = strlen(src);
		if (l > 3 && src[l - 3] == '.' && src[l - 2] == 'g' &&
		    src[l - 1] == 'z') {
			str_copy(dst, src);
			dst[l - 3] = '\0';
		} else {
			con_write("usage: gunzip <src> [dst]\n");
			kfree(in);
			return;
		}
	}

	uint8_t *out = kmalloc(orig ? (uint32_t)orig : 1);
	if (!out) {
		con_write("gunzip: out of memory\n");
		kfree(in);
		return;
	}
	int dlen = gzip_decompress(in, (uint32_t)n, out, (uint32_t)orig);
	if (dlen < 0) {
		con_write("gunzip: corrupt or unsupported gzip\n");
	} else if (fs_create(dst, out, (uint32_t)dlen) != 0) {
		con_write("gunzip: write failed\n");
	} else {
		con_write(src); con_write(" -> "); con_write(dst);
		con_write(" ("); con_write_dec((uint32_t)dlen);
		con_write(" bytes)\n");
	}
	kfree(in); kfree(out);
}

#define REDIR_MAX (16 * 1024)  /* cap on captured output for "cmd > file" */

/* ---- built-in command table ---------------------------------------------
 * Adapters give every handler the uniform cmd_fn shape (one const char *arg).
 * No-arg commands ignore it; a few carry the small bit of glue that used to
 * sit inline in run()'s dispatch ladder. Handlers already taking a single
 * const char * go into the table directly. */
static void bi_clear(const char *a)    { (void)a; con_clear(); }
static void bi_journal(const char *a)  { (void)a; journal_dump(); }
static void bi_uptime(const char *a)
{
	(void)a;
	uint32_t s = timer_seconds();
	uint32_t h = s / 3600; s %= 3600;
	uint32_t m = s / 60;   s %= 60;
	con_write("up ");
	if (h) { con_write_dec(h); con_write("h "); }
	if (h || m) { con_write_dec(m); con_write("m "); }
	con_write_dec(s); con_write("s\n");
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
		con_write("ls: path too long\n");
	else
		fs_list(rpath);
}
static void bi_pwd(const char *a)      { (void)a; cmd_pwd(); }
static void bi_format(const char *a)   { (void)a; fs_format(); con_write("filesystem erased.\n"); }
static void bi_ethdiag(const char *a)  { (void)a; e1000_diag(); }
static void bi_dhcp(const char *a)     { (void)a; net_dhcp_now(); }
static void bi_txtest(const char *a)   { (void)a; e1000_txtest(); }
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

/* ---- mtp: browse a phone/camera attached in USB file-transfer (MTP) mode ----
 * MTP exposes objects (files + folders), not blocks, so it isn't mounted at
 * /media -- it's walked here, handle by handle, much like `lsusb`/`torrent`. */
static void mtp_put_hex(uint32_t v)
{
	static const char hx[] = "0123456789abcdef";
	con_write("0x");
	for (int s = 28; s >= 0; s -= 4)
		con_putc(hx[(v >> s) & 0xF]);
}
static void mtp_list_cb(uint32_t handle, const char *name, uint32_t size,
                        int is_dir, void *ctx)
{
	(void)ctx;
	mtp_put_hex(handle);
	if (is_dir) {
		con_write("  <dir>  ");
	} else {
		con_write("  ");
		con_write_dec(size);
		con_write(" bytes  ");
	}
	con_write(name);
	con_putc('\n');
}
/* Parse an object handle, accepting 0x-prefixed hex or plain decimal. */
static uint32_t mtp_parse_handle(const char *a)
{
	uint32_t v = 0;
	if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
		for (a += 2; *a; a++) {
			char c = *a; int d;
			if (c >= '0' && c <= '9') d = c - '0';
			else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
			else break;
			v = v * 16 + (uint32_t)d;
		}
	} else {
		for (; *a >= '0' && *a <= '9'; a++)
			v = v * 10 + (uint32_t)(*a - '0');
	}
	return v;
}
static void cmd_mtp(const char *arg)
{
	while (*arg == ' ') arg++;
	if (!xhci_mtp_present()) {
		con_write("mtp: no device (attach a phone/camera in file-transfer mode)\n");
		return;
	}
	if (arg[0] == '\0' || streq(arg, "info")) {
		con_write("mtp: ");
		con_write(xhci_mtp_model());
		con_putc('\n');
		return;
	}

	/* Split off the sub-command word. */
	char sub[8];
	int i = 0;
	while (arg[i] && arg[i] != ' ' && i < (int)sizeof(sub) - 1) { sub[i] = arg[i]; i++; }
	sub[i] = '\0';
	const char *rest = arg + i;
	while (*rest == ' ') rest++;

	if (streq(sub, "ls")) {
		uint32_t parent = rest[0] ? mtp_parse_handle(rest) : XHCI_MTP_ROOT;
		int n = xhci_mtp_list(parent, mtp_list_cb, 0);
		if (n < 0)       con_write("mtp: list failed\n");
		else if (n == 0) con_write("(empty)\n");
		return;
	}
	if (streq(sub, "get")) {
		if (rest[0] == '\0') { con_write("usage: mtp get <handle> <dest>\n"); return; }
		uint32_t handle = mtp_parse_handle(rest);
		while (*rest && *rest != ' ') rest++;       /* skip the handle token       */
		while (*rest == ' ') rest++;
		if (rest[0] == '\0') { con_write("usage: mtp get <handle> <dest>\n"); return; }
		char dst[PATH_MAX];
		if (path_resolve(rest, dst) != 0) { con_write("mtp: bad path\n"); return; }
		con_write("mtp: downloading "); mtp_put_hex(handle);
		con_write(" -> "); con_write(rest); con_write(" ...\n");
		if (xhci_mtp_get(handle, dst) == 0)
			con_write("mtp: done\n");
		else
			con_write("mtp: get failed\n");
		return;
	}
	con_write("usage: mtp [info] | mtp ls [handle] | mtp get <handle> <dest>\n");
}

/* Safely detach the removable volume mounted at /media so the medium can be removed.
 * Writes are write-through, so there is nothing buffered to flush -- this just
 * drops the mount. `mount` re-probes a non-owofs disk and remounts. */
static void bi_eject(const char *a)
{
	(void)a;
	const char *type = NULL, *label = "";
	void (*unmount)(void) = NULL;
	if (iso_mounted())      { type = iso_type(); label = iso_label(); unmount = iso_unmount; }
	else if (ext_mounted()) { type = ext_type(); label = ext_label(); unmount = ext_unmount; }
	else if (hfs_mounted()) { type = hfs_type(); label = hfs_label(); unmount = hfs_unmount; }
	else if (fat_mounted()) { type = fat_type(); label = fat_label(); unmount = fat_unmount; }
	if (!unmount) {
		con_write("eject: nothing mounted at /media\n");
		return;
	}
	con_write("eject: detaching ");
	con_write(type);
	if (label[0]) {
		con_write(" \"");
		con_write(label);
		con_write("\"");
	}
	con_write(" from /media -- safe to remove\n");
	unmount();
}

static void bi_mount(const char *a)
{
	(void)a;
	if (fat_mounted() || hfs_mounted() || ext_mounted() || iso_mounted()) {
		con_write("mount: a volume is already mounted at /media\n");
		return;
	}
	if (blk_mount_fat() != 0)
		con_write("mount: no removable volume found\n");
}
/* `play` is now the ring-3 program /bin/play (streaming WAV player); the kernel
 * no longer has a play builtin. is_capture_cli() routes it like neofetch/curl. */
static void bi_cpuinfo(const char *a)  { (void)a; cmd_cpuinfo(); }
static void bi_whoami(const char *a)   { (void)a; cmd_whoami(); }
static void bi_which(const char *a)    { cmd_which((char *)a); }
static void bi_date(const char *a)     { (void)a; cmd_date(); }
static void bi_ntp(const char *a)      { net_ntp(a); }
static void bi_fstest(const char *a)     { (void)a; fs_selftest(); }
static void bi_cryptotest(const char *a) { (void)a; crypto_selftest(); }
static void bi_echo(const char *a)       { con_write(a); con_putc('\n'); }
static void bi_poweroff(const char *a)
{
	(void)a;
	con_write("powering off...\n");
	power_off();
	con_write("poweroff not supported here\n");
}
static void bi_restart(const char *a)    { (void)a; con_write("restarting...\n"); power_reset(); }

/* The one and only command list. Add a built-in here — that wires it into
 * dispatch, `which`, and `help` at once. Order is the order `help` prints.
 * fn == NULL rows are help-only notes for things that aren't bare commands. */
static const struct builtin builtins[] = {
	{ "help",     cmd_help,     "help          show this list" },
	{ "clear",    bi_clear,     "clear         clear the screen" },
	{ "journal",  bi_journal,   "journal       replay the verbose boot log" },
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
	{ "format",   bi_format,    "format        erase the filesystem" },
	{ "ethdiag",  bi_ethdiag,   "ethdiag       Intel NIC bring-up log + link/traffic stats" },
	{ "dhcp",     bi_dhcp,      "dhcp          re-request a DHCP lease (waits for link)" },
	{ "txtest",   bi_txtest,    "txtest        Intel NIC transmit self-test (MAC loopback)" },
	{ "lsusb",    bi_lsusb,     "lsusb [port]  list USB devices (port: detail for one)" },
	{ "mtp",      cmd_mtp,      "mtp [info]    browse a phone/camera in file-transfer mode\n"
	                            "  mtp ls [handle]  list the store root, or a folder by handle\n"
	                            "  mtp get <handle> <dest>  download an object to owofs" },
	{ "storage",  bi_storage,   "storage       list storage devices (IDE/SATA/NVMe)" },
	{ "mount",    bi_mount,     "mount         mount a removable volume (USB/SD/disk) at /media" },
	{ "eject",    bi_eject,     "eject         safely detach the removable volume at /media" },
	{ NULL,       NULL,         "play [arg]    play a .wav file, or a tune (twinkle/megalovania/sweden)" },
	{ "cpuinfo",  bi_cpuinfo,   "cpuinfo       CPU vendor, model, and feature flags" },
	{ "uname",    cmd_uname,    "uname [-a]    system identity" },
	{ "whoami",   bi_whoami,    "whoami        current user" },
	{ "which",    bi_which,     "which <name>  locate a command (built-in or file)" },
	{ "loadkeys", cmd_loadkeys, "loadkeys <l>  set keyboard layout (us, fr)" },
	{ "date",     bi_date,      "date          current date and time" },
	{ "ntp",      bi_ntp,       "ntp [host] [+/-tz] set the clock from an NTP server (tz hours)" },
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

/* Ring-3 CLI tools whose console output the terminal captures and renders inline
 * (like a kernel builtin), rather than launching them as a detached window. A
 * GUI /bin app must NOT be listed — capturing it would block until it exits,
 * which it never does. Misnaming only loses output, so this is the safe list. */
static int is_capture_cli(const char *name)
{
	static const char *const cli[] = { "neofetch", "curl", "lspci", "play", "ping", "dig" };
	for (unsigned i = 0; i < sizeof cli / sizeof cli[0]; i++)
		if (streq(name, cli[i]))
			return 1;
	return 0;
}

/* Spawn a /bin CLI program and capture its console output into `buf`, blocking
 * until it exits. The child runs in its OWN address space, so con_putc can't
 * reach the caller's user buffer — capture into a kernel buffer and copy across
 * at the end (we are back in the caller's CR3 by then). Returns bytes, or -1. */
static int run_bin_capture(const char *cmd, char *buf, int max)
{
	static char kbuf[48 * 1024];        /* kernel BSS: mapped under every CR3   */
	char line[256];
	int k = 0;
	for (; cmd[k] && k < (int)sizeof line - 1; k++)
		line[k] = cmd[k];
	line[k] = '\0';

	char *argv[8];
	int argc = tokenize(line, argv, 8);
	if (argc == 0)
		return -1;

	con_capture_begin_ansi(kbuf, sizeof kbuf);
	int pid = spawn(argc, argv);
	if (pid < 0) {
		con_capture_end();
		return -1;
	}
	int status;
	while (!sched_try_reap(pid, &status)) {
		if (wm_focus_takes_keys()) {
			char c;
			while (keyboard_poll(&c)) wm_feed_key(c);
			while (serial_poll(&c))   wm_feed_key(c);
		}
		__asm__ volatile ("sti; hlt");
	}
	int n = (int)con_capture_end();
	if (n > max) n = max;
	for (int i = 0; i < n; i++)
		buf[i] = kbuf[i];
	return n;
}

/* Run a command with its console output captured into `buf`, for the ring-3
 * terminal's bridge syscall: it reuses every kernel builtin (journal, lspci,
 * date, ...) AND every ring-3 CLI tool (neofetch, curl) without each being
 * re-implemented or given its own syscall. Returns the captured byte count, or
 * -1 if the first token is neither a builtin nor a capturable /bin program (so
 * the caller can launch it as a detached GUI app instead). */
int shell_exec_capture(const char *cmd, char *buf, int max)
{
	char name[64];
	int i = 0;
	while (cmd[i] == ' ') i++;
	int n = 0;
	while (cmd[i] && cmd[i] != ' ' && n < (int)sizeof name - 1)
		name[n++] = cmd[i++];
	name[n] = '\0';
	const struct builtin *b = builtin_find(name);
	if (!b)
		return is_capture_cli(name) ? run_bin_capture(cmd, buf, max) : -1;
	const char *arg = cmd + i;
	while (*arg == ' ') arg++;
	con_capture_begin_ansi(buf, (uint32_t)max);   /* keep colors for the terminal */
	b->fn(arg);
	return (int)con_capture_end();
}

/* ---- streaming command bridge (ring-3 terminal) -------------------------- */
/* shell_exec_capture buffers a whole command, so the terminal freezes on a long
 * or voluminous one (curl, ping) until it finishes, then dumps everything and is
 * also capped by the user buffer. The streaming bridge instead runs the command
 * in a persistent producer thread whose console output flows into a ring; the
 * terminal drains the ring in chunks and renders as it goes. Kernel threads
 * can't be reaped, so the runner is created once (shell_stream_start) and parked
 * between commands. Single producer (runner/IRQ) + single consumer (terminal),
 * so the ring is lock-free apart from a cli around the head update (a stray IRQ
 * write can race the runner). */
#define CMDR_RING_SZ 32768
static char              cmdr_ring[CMDR_RING_SZ];
static volatile uint32_t cmdr_w, cmdr_r;     /* free-running write / read counts  */
static volatile int      cmdr_arm;           /* a command is queued or running    */
static volatile int      cmdr_done;          /* producer finished; drain then EOF */
static volatile int      cmdr_kill;          /* abort requested (Ctrl+C / close)  */
static volatile int      cmdr_intr;          /* graceful interrupt already sent    */
static volatile int      cmdr_child;         /* pid of the running CLI child, or 0 */
static char              cmdr_cmd[256];
static char              cmdr_cwd[256];

/* con-capture sink: append one byte to the ring. The producer thread (IF=1)
 * blocks for space when the ring is full so nothing is lost; a write from an
 * IF-off context (a stray IRQ log) can't yield, so it drops rather than hang. */
static void cmdr_sink(char c)
{
	for (;;) {
		uint32_t fl;
		__asm__ volatile ("pushf; pop %0; cli" : "=r"(fl));
		if (cmdr_w - cmdr_r < CMDR_RING_SZ) {
			cmdr_ring[cmdr_w % CMDR_RING_SZ] = c;
			cmdr_w++;
			if (fl & 0x200) __asm__ volatile ("sti");
			return;
		}
		if (!(fl & 0x200)) return;            /* IF-off: can't drain, drop      */
		__asm__ volatile ("sti");
		sched_yield();                        /* let the terminal drain the ring */
	}
}

/* Run one command with its output streamed to the ring -- builtin (in this
 * thread) or a capturable /bin CLI tool (a child process; its output is global,
 * so it lands in the ring too while we reap it). */
static void cmdr_run(const char *cmd)
{
	char name[64];
	int i = 0;
	while (cmd[i] == ' ') i++;
	int n = 0;
	while (cmd[i] && cmd[i] != ' ' && n < (int)sizeof name - 1)
		name[n++] = cmd[i++];
	name[n] = '\0';

	con_capture_begin_sink(cmdr_sink);
	const struct builtin *b = builtin_find(name);
	if (b) {
		const char *arg = cmd + i;
		while (*arg == ' ') arg++;
		b->fn(arg);
	} else {                                  /* a capturable /bin CLI tool      */
		char line[256];
		int k = 0;
		for (; cmd[k] && k < (int)sizeof line - 1; k++)
			line[k] = cmd[k];
		line[k] = '\0';
		char *argv[8];
		int argc = tokenize(line, argv, 8);
		int pid = argc > 0 ? spawn(argc, argv) : -1;
		if (pid >= 0) {
			cmdr_child = pid;             /* let an abort target it          */
			int status;
			while (!sched_try_reap(pid, &status)) {
				if (cmdr_kill) sched_kill(pid);
				__asm__ volatile ("sti; hlt");
			}
			cmdr_child = 0;
		}
	}
	con_capture_end();
}

/* Persistent producer thread: park until shell_stream_begin arms a command, run
 * it, signal EOF, then wait for the terminal to acknowledge before re-arming. */
static void cmd_runner(void)
{
	for (;;) {
		while (!cmdr_arm) __asm__ volatile ("sti; hlt");
		sched_set_cwd_self(cmdr_cwd);         /* resolve relative paths like the bridge */
		cmdr_run(cmdr_cmd);
		cmdr_done = 1;                         /* terminal drains, then EOFs us   */
		while (cmdr_arm) __asm__ volatile ("sti; hlt");
	}
}

void shell_stream_start(void)
{
	sched_create_thread("cmdrunner", cmd_runner);
}

/* Begin streaming `cmd`. Returns 0 if it is a streamable command (builtin or a
 * capturable CLI tool) -- the caller then drains with shell_stream_read; -1 if
 * not, so the caller can launch it as a detached GUI app instead. One stream at
 * a time (single ring + runner). */
int shell_stream_begin(const char *cmd)
{
	if (cmdr_arm)
		return -1;                            /* one at a time                   */
	char name[64];
	int i = 0;
	while (cmd[i] == ' ') i++;
	int n = 0;
	while (cmd[i] && cmd[i] != ' ' && n < (int)sizeof name - 1)
		name[n++] = cmd[i++];
	name[n] = '\0';
	if (!is_builtin(name) && !is_capture_cli(name))
		return -1;

	int k = 0;
	for (; cmd[k] && k < (int)sizeof cmdr_cmd - 1; k++)
		cmdr_cmd[k] = cmd[k];
	cmdr_cmd[k] = '\0';
	str_copy(cmdr_cwd, sched_cwd());
	cmdr_w = cmdr_r = 0;
	cmdr_done = 0;
	cmdr_kill = 0;
	cmdr_intr = 0;
	cmdr_child = 0;
	cmdr_arm = 1;                             /* wake the runner                 */
	return 0;
}

/* Whether a streamed command is currently running. */
int shell_stream_active(void)
{
	return cmdr_arm;
}

/* Ctrl+C on a running command. Two-stage, Linux-like: the first press echoes
 * "^C" (the terminal's job, not each program's — so curl/ping/torrent stay
 * silent) and delivers a cooperative interrupt to a CLI child so it can tear
 * down its own TCP/TLS and exit cleanly; a second press (or a builtin, which
 * can't observe the latch) hard-kills. The "^C" goes through the active capture
 * sink, so it lands in the terminal that owns the command. */
void shell_stream_interrupt(void)
{
	if (!cmdr_intr) {
		cmdr_intr = 1;
		con_write("^C\n");
		if (cmdr_child > 0)
			sched_set_intr(cmdr_child);   /* graceful: curl/ping/... abort   */
		else
			cmdr_kill = 1;                /* builtin: best-effort            */
		return;
	}
	cmdr_kill = 1;                            /* second Ctrl+C: force          */
	if (cmdr_child > 0)
		sched_kill(cmdr_child);
}

/* Abort the running command without the "^C" echo (terminal close): hard-kill a
 * CLI child so the runner's reap returns and the stream resets promptly. */
void shell_stream_abort(void)
{
	cmdr_kill = 1;
	if (cmdr_child > 0)
		sched_kill(cmdr_child);
}

/* Drain up to `max` bytes of the running command's output into `buf`. Returns
 * the byte count (>0), 0 if it is still running with nothing buffered yet, or -1
 * at end of output (which also releases the runner to park). */
int shell_stream_read(char *buf, int max)
{
	uint32_t avail = cmdr_w - cmdr_r;
	if (avail) {
		int n = (int)avail;
		if (n > max) n = max;
		for (int i = 0; i < n; i++)
			buf[i] = cmdr_ring[(cmdr_r + (uint32_t)i) % CMDR_RING_SZ];
		cmdr_r += (uint32_t)n;
		return n;
	}
	if (cmdr_done) {
		cmdr_arm = 0;                         /* let the runner re-arm           */
		return -1;
	}
	return 0;
}

static void cmd_help(const char *a)
{
	(void)a;
	con_write("commands:\n");
	for (const struct builtin *b = builtins; b->name || b->help; b++) {
		if (!b->help)
			continue;
		con_write("  ");
		con_write(b->help);
		con_putc('\n');
	}
}

static void run(char *cmd)
{
	if (cmd[0] == '\0')
		return;                /* blank line                              */

	/* "<cmd> > <file>": capture cmd's text output and write it to file.
	 * Works for any built-in because all of them emit through con_putc. */
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
			con_write("usage: <command> > <file>\n");
			return;
		}
		char *cap = kmalloc(REDIR_MAX);
		if (!cap) {
			con_write("redirect: out of memory\n");
			return;
		}
		con_capture_begin(cap, REDIR_MAX);
		run(cmd);                              /* no '>' left: dispatches  */
		uint32_t n = con_capture_end();
		if (fs_create(fname, cap, n) == 0) {
			con_write("wrote ");
			con_write_dec(n);
			con_write(" bytes to ");
			con_write(fname);
			con_putc('\n');
		} else {
			con_write("redirect: write failed\n");
		}
		kfree(cap);
		return;
	}

	/* Pipe "A | B". Two cases by what B is:
	 *  - B is a program  -> run A and B concurrently through a real kernel pipe
	 *    (cmd_run); A's stdout becomes B's stdin. This is how program pipelines
	 *    work now that there is no `run` keyword.
	 *  - B is a builtin (grep) -> capture A's console output and hand it to B as
	 *    piped input it reads from g_pipe_in. Chains: A | grep x | grep y. */
	{
		char *bar = cmd;
		while (*bar && *bar != '|')
			bar++;
		if (*bar == '|') {
			/* Peek B's first token without disturbing cmd. */
			char *rp = bar + 1;
			while (*rp == ' ')
				rp++;
			char rname[64];
			int rn = 0;
			while (rp[rn] && rp[rn] != ' ' && rn < (int)sizeof(rname) - 1) {
				rname[rn] = rp[rn];
				rn++;
			}
			rname[rn] = '\0';
			if (!builtin_find(rname)) {        /* B is a program: real pipe   */
				cmd_run(cmd);                  /* splits on '|', spawns both    */
				return;
			}
			*bar = '\0';
			char *left = cmd, *right = bar + 1;
			while (*right == ' ')
				right++;
			char *lend = bar;              /* trim blanks before '|'   */
			while (lend > left && lend[-1] == ' ')
				*--lend = '\0';
			if (left[0] == '\0' || right[0] == '\0') {
				con_write("usage: <command> | grep <pattern>\n");
				return;
			}
			char *cap = kmalloc(REDIR_MAX);
			if (!cap) {
				con_write("pipe: out of memory\n");
				return;
			}
			con_capture_begin(cap, REDIR_MAX);
			run(left);
			uint32_t n = con_capture_end();
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
		con_write("unknown command: ");
		con_write(av[0]);
		con_write("\ntype 'help'\n");
	}
}

void shell_feed(char c)
{
	switch (c) {
	case '\n':
		con_set_pos((uint16_t)(edit_start + line_len));  /* end of line     */
		con_putc('\n');
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
				con_putc('\b');
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
			con_set_pos((uint16_t)(edit_start + --cursor));
		return;
	case KEY_RIGHT:
		if (cursor < line_len)
			con_set_pos((uint16_t)(edit_start + ++cursor));
		return;
	case KEY_HOME:
		cursor = 0;
		con_set_pos(edit_start);
		return;
	case KEY_END:
		cursor = line_len;
		con_set_pos((uint16_t)(edit_start + line_len));
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
		con_clear();
		print_prompt();
		edit_start = con_get_pos();
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
				uint32_t s0 = con_scroll_count();
				con_putc(c);
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
	con_set_pos((uint16_t)(edit_start + line_len));
	con_write("^C\n");
	prompt();
}

void shell_init(void)
{
	/* The shell now owns the console: stop journaling so interactive traffic
	 * isn't logged, and clear the screen so the terminal opens clean -- whether
	 * the boot was quiet (nothing drawn) or verbose (boot log still on screen).
	 * The bring-up log is in the journal; `journal` replays it. */
	journal_capture(0);
	con_clear();
	con_write("type 'help' for commands, 'journal' for the boot log.\n");
	prompt();
}

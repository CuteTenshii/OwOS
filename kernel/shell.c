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
#include "keyboard.h"
#include "serial.h"
#include "vmtools.h"

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
		con_printf("~%s", cwd + hlen);         /* the path below home, if any */
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
	con_printf("%s%u KiB\n", label, kib);
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
		con_printf("%u.%u GiB", tenths / 10, tenths % 10);
	} else {
		con_printf("%u MiB", mib);
	}
}

static void cmd_storage(void)
{
	uint8_t buf[512];
	int n = 0;

	if (ata_present()) {
		con_printf("  IDE/ATA    %s  ", ata_model());
		write_capacity(ata_sectors());
		con_write(" (PIO, primary master)\n");
		n++;
	}
	if (ahci_present()) {
		con_printf("  SATA/AHCI  %s  ", ahci_model());
		write_capacity(ahci_sectors());
		con_write(" (DMA) -- read LBA0: ");
		con_write(ahci_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (nvme_present()) {
		con_printf("  NVMe       %s  ", nvme_model());
		write_capacity(nvme_sectors());
		con_write(" (ns 1) -- read LBA0: ");
		con_write(nvme_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
		n++;
	}
	if (xhci_msc_present()) {
		con_printf("  USB        %s  ", xhci_msc_model());
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

/* Split "prog a b c" in place into argv; returns argc (argv[0] is the prog).
 * Honors single/double quotes so an argument may contain spaces, e.g.
 *   curl -H "X-Test: a b"   ->  argv = {"curl","-H","X-Test: a b"}
 * The quote characters are removed and each token is compacted in place (the
 * write head never passes the read head, so this stays a safe in-place edit). */
static int tokenize(char *s, char **argv, int max)
{
	int argc = 0;
	while (argc < max) {
		while (*s == ' ')
			s++;
		if (!*s)
			break;
		argv[argc++] = s;
		char *w = s;                       /* compacted write head (<= s)      */
		char q = 0;                        /* open quote char, or 0            */
		while (*s) {
			char c = *s;
			if (q) {                       /* inside quotes: copy until close  */
				if (c == q) q = 0;
				else        *w++ = c;
				s++;
			} else if (c == '"' || c == '\'') {
				q = c; s++;                /* open a quote (dropped)           */
			} else if (c == ' ') {
				break;                     /* unquoted space ends the token    */
			} else {
				*w++ = c; s++;
			}
		}
		if (*s == ' ') s++;                /* consume the delimiter            */
		*w = '\0';                         /* terminate the compacted token    */
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
		/* While a foreground child runs, the recovery console's main loop is parked
		 * here; leave keystrokes in the ring for a console program's own
		 * read()/readline. (The desktop's keyboard is drained by the user-space
		 * compositor, not this path.) */
		__asm__ volatile ("sti; hlt");
	}
	con_printf("[pid %u exited, status %u]\n", (uint32_t)pid, (uint32_t)status);
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
			con_printf("%s\n", line_start);
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

/* Parse a dotted-quad "a.b.c.d" into ip[4]; returns the pointer past it (at
 * the next space or NUL), or NULL on a malformed address. */



/* torrent get <file> — download a whole single-file torrent concurrently from
 * several peers at once, verifying each piece's SHA-1 into a pre-allocated
 * output file. */

/* torrent info <file>   — parse a .torrent and print its metainfo + infohash.
 * torrent peers <file>  — announce to its tracker and list the returned peers.
 * torrent peer <f> <ip> <port> — leech piece 0 from one peer, verify its SHA-1.
 * torrent get <file>    — download the whole torrent to disk (single-file).
 * torrent test          — parse an embedded known-good torrent (offline).
 * torrent peers test     — tracker reply parsing self-check (offline).         */




/* System identity, the fields a POSIX uname reports. */
#define UNAME_SYSNAME  "OwOS"                 /* -s kernel name              */
#define UNAME_NODENAME "owos"                 /* -n network node name        */
#define UNAME_RELEASE  "0.1"                  /* -r kernel release           */
#define UNAME_MACHINE  "i386"                 /* -m hardware                 */

/* "uname [-asnrmo]" — print system identity. Bare/-s is the kernel name;
 * -a is everything in the conventional sysname/nodename/release/machine order. */


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
		con_printf("cd: no such directory: %s\n", a);
		return;
	}
	if (d == 0) {
		con_printf("cd: not a directory: %s\n", a);
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
	con_printf("%s\n", cwd);
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
		con_printf("cp: no such file: %s\n", src);
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
		con_printf("%s -> %s\n", rsrc, rdst);
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
	con_printf("%s -> %s\n", rsrc, rdst);
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
			con_printf("removed %s\n", rpath);
		} else if (r == -2) {
			con_printf("rm: is a directory (use rmdir): %s\n", name);
		} else {
			con_printf("rm: no such file: %s\n", name);
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
		con_printf("created %s\n", rpath);
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
			con_printf("removed %s\n", rpath);
		} else if (r == -2) {
			con_printf("rmdir: not a directory: %s\n", name);
		} else if (r == -3) {
			con_printf("rmdir: directory not empty: %s\n", name);
		} else {
			con_printf("rmdir: no such directory: %s\n", name);
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


/* "unzip <src> [dst]" — restore a zipped file. dst defaults to <src> minus a
 * trailing ".z"; without that suffix an explicit dst is required. */

/* "gzip <src> [dst]" — compress a file to standard gzip. dst defaults to
 * "<src>.gz"; the output is readable by host gunzip. */

/* "gunzip <src> [dst]" — decompress a gzip file (from OwOS or host gzip). dst
 * defaults to <src> minus a trailing ".gz". */

#define REDIR_MAX (16 * 1024)  /* cap on captured output for "cmd > file" */

/* ---- built-in command table ---------------------------------------------
 * Adapters give every handler the uniform cmd_fn shape (one const char *arg).
 * No-arg commands ignore it; a few carry the small bit of glue that used to
 * sit inline in run()'s dispatch ladder. Handlers already taking a single
 * const char * go into the table directly. */
static void bi_clear(const char *a)    { (void)a; con_clear(); }
static void bi_journal(const char *a)  { (void)a; journal_dump(); }
static void bi_meminfo(const char *a)  { (void)a; cmd_meminfo(); }
static void bi_ps(const char *a)       { (void)a; sched_dump(); }
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
static void bi_storage(const char *a)  { (void)a; cmd_storage(); }

/* ---- mtp: browse a phone/camera attached in USB file-transfer (MTP) mode ----
 * MTP exposes objects (files + folders), not blocks, so it isn't mounted at
 * /media -- it's walked here, handle by handle, much like `lsusb`/`torrent`. */

/* Safely detach the removable volume mounted at /media so the medium can be removed.
 * Writes are write-through, so there is nothing buffered to flush -- this just
 * drops the mount. `mount` re-probes a non-owofs disk and remounts. */
/* Build the default mount point "/media/<dev>" into out. */
static void media_point(const char *dev, char *out)
{
	const char *pfx = "/media/";
	int i = 0;
	for (; pfx[i]; i++) out[i] = pfx[i];
	for (int j = 0; dev[j] && i < 60; j++) out[i++] = dev[j];
	out[i] = '\0';
}

/* umount [point]: detach the volume at `point`, or the first mount if no arg. */
static void bi_umount(const char *a)
{
	while (*a == ' ') a++;
	if (*a) {
		if (fs_unmount(a) == 0) { con_printf("umount: detached %s\n", a); }
		else                      con_write("umount: nothing mounted there\n");
		return;
	}
	struct fs_mountinfo mi;
	if (fs_mount_enum(0, &mi) == 1) {
		fs_unmount(mi.point);
		con_printf("umount: detached %s\n", mi.point);
	} else con_write("umount: nothing mounted\n");
}

/* mount [dev [point]]: mount a removable device, or the first unmounted one. */
static void bi_mount(const char *a)
{
	while (*a == ' ') a++;
	char dev[16] = "", point[64] = "";
	int i = 0;
	while (*a && *a != ' ' && i < 15) dev[i++] = *a++;
	dev[i] = '\0';
	while (*a == ' ') a++;
	i = 0;
	while (*a && *a != ' ' && i < 63) point[i++] = *a++;
	point[i] = '\0';

	if (dev[0]) {
		if (!point[0]) media_point(dev, point);
		if (fs_mount(dev, point) == 0) { con_printf("mount: %s -> %s\n", dev, point); }
		else                             con_write("mount: cannot mount (unknown device or filesystem)\n");
		return;
	}
	struct blk_devinfo devs[8];
	int n = blk_dev_list(devs, 8);
	for (int k = 0; k < n; k++) {
		struct fs_mountinfo mi;
		int mounted = 0;
		for (int j = 0; fs_mount_enum(j, &mi); j++)
			if (streq(mi.dev, devs[k].dev)) { mounted = 1; break; }
		if (!mounted) {
			media_point(devs[k].dev, point);
			if (fs_mount(devs[k].dev, point) == 0) { con_printf("mount: %s -> %s\n", devs[k].dev, point); }
			else                                     con_printf("mount: no recognised filesystem on %s\n", devs[k].dev);
			return;
		}
	}
	con_write("mount: no unmounted device (try `ls /dev`)\n");
}
/* `play` is now the program /bin/play (streaming WAV player); the kernel
 * no longer has a play builtin. is_capture_cli() routes it like neofetch/curl. */
static void bi_echo(const char *a)       { con_printf("%s\n", a); }
/* clip: with text, copy it to the clipboard (pushed to the host on VMware);
 * with no argument, print the current clipboard contents. */
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
/* The recovery console is deliberately minimal: enough to inspect and repair the
 * system over COM1 when the desktop isn't up. Everything else (info commands,
 * codecs, clipboard, torrent, ...) lives in the
 * terminal `/bin/term`. A bare non-builtin name still spawns a /bin program, so
 * `curl`, `play`, `neofetch`, etc. remain runnable here -- they're just not
 * listed. `mtp` stays because it's kernel-only with nowhere else to live. */
static const struct builtin builtins[] = {
	{ "help",     cmd_help,     "help          show this list" },
	{ "clear",    bi_clear,     "clear         clear the screen" },
	{ "journal",  bi_journal,   "journal       replay the verbose boot log" },
	{ "meminfo",  bi_meminfo,   "meminfo       physical and heap memory usage" },
	{ "ps",       bi_ps,        "ps            list scheduler threads and run counts" },
	{ "ls",       bi_ls,        "ls [path]     list a directory (default: current)" },
	{ "pwd",      bi_pwd,       "pwd           print the working directory" },
	{ "cd",       cmd_cd,       "cd [dir]      change directory (no arg -> home)" },
	{ "cat",      cmd_cat,      "cat <file>    print a file" },
	{ "grep",     cmd_grep,     "grep <p> <f>  print lines of <f> that contain <p>" },
	{ "cp",       cmd_cp,       "cp <src> <dst> copy a file" },
	{ "mv",       cmd_mv,       "mv <src> <dst> rename/move a file or directory" },
	{ "rm",       cmd_rm,       "rm <file>...  delete files" },
	{ "mkdir",    cmd_mkdir,    "mkdir <dir>...  create directories" },
	{ "rmdir",    cmd_rmdir,    "rmdir <dir>...  remove empty directories" },
	{ "storage",  bi_storage,   "storage       list storage devices (IDE/SATA/NVMe)" },
	{ "mount",    bi_mount,     "mount [dev [pt]]  mount a removable device (see ls /dev) at /media/<name>" },
	{ "umount",   bi_umount,    "umount [point]    detach a mounted volume" },
	{ "echo",     bi_echo,      "echo <text>   print text back" },
	{ "poweroff", bi_poweroff,  "poweroff      shut the machine down" },
	{ "restart",  bi_restart,   "restart       reboot the machine" },
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



static void cmd_help(const char *a)
{
	(void)a;
	con_write("commands:\n");
	for (const struct builtin *b = builtins; b->name || b->help; b++) {
		if (!b->help)
			continue;
		con_printf("  %s\n", b->help);
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
			con_printf("wrote %u bytes to %s\n", n, fname);
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
	 * name (cwd, then /bin) and run it in user space with its arguments. This is
	 * how every user program (hello, args, curl, ...) launches by bare name;
	 * none of them is wired into the command table above. */
	char *av[8] = {0};
	int ac = tokenize(cmd, av, 8);
	int pid = (ac > 0) ? spawn(ac, av) : -1;
	if (pid >= 0) {
		wait_reap(pid);
	} else if (ac > 0) {
		con_printf("unknown command: %s\ntype 'help'\n", av[0]);
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

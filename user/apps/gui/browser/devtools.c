/* devtools.c — the browser's in-window developer tools (F12 / Ctrl+Shift+I).
 *
 * A bottom-docked overlay with three views, switched by a tab strip:
 *   Elements  the live DOM tree, one node per line, click to select
 *   Styles    the computed CSS of the selected element (via the css.c engine)
 *   Console   the page's console.* output and uncaught JS errors
 *
 * Threading: the live DOM is owned by the worker thread (it parses, mutates and
 * frees it), so the DOM/Styles snapshots are built there in devtools_worker_tick
 * — never walked from the main thread. The console buffer is appended from the
 * worker (js.c routes console.* + errors here) and read by the main thread's
 * render. A single spinlock (dt_lk) guards the snapshot text and the console ring
 * against the render thread; it is the same xchg pattern as main.c's blk_lock,
 * but a separate lock so console/DOM updates never serialise against the page
 * buffers. The main thread only reads the prepared snapshots and draws them. */
#include "browser.h"
#include "css.h"

/* --- the snapshot/console interlock (worker writes, main reads) ------------ */
static volatile int dt_lk;
static inline int dt_xchg(volatile int *p, int v)
{
	__asm__ volatile ("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
	return v;
}
static void dt_lock(void)   { while (dt_xchg(&dt_lk, 1)) msleep(0); }
static void dt_unlock(void) { __asm__ volatile ("" ::: "memory"); dt_lk = 0; }

/* --- panel state ----------------------------------------------------------- */
enum { TAB_ELEMENTS, TAB_STYLES, TAB_CONSOLE, TAB_NETWORK, N_TABS };
typedef char dt_panel_count_matches[N_TABS == DT_PANELS ? 1 : -1];  /* keep dt_view in sync */
static int dt_open;
static int dt_tab;
static int dt_scroll[N_TABS];
static volatile int dt_dirty;       /* main asks the worker to rebuild snapshots  */
static unsigned dt_seen_seq;        /* last B->seq the snapshot was built for      */

#define DT_HDR   22                 /* tab-strip height at the panel top           */
/* A code view is monospace: rows use the fixed cell font via gfx_text_utf8, which
 * decodes UTF-8 (so accents render) while keeping one column per character. */
#define ROW_H    GLYPH_H           /* monospace cell height (16)                    */

/* --- a growable text buffer with a per-line offset index ------------------- *
 * Each snapshot (DOM tree, styles) is one char buffer holding '\n'-separated
 * lines, plus an offsets[] array so the renderer indexes a line in O(1). */
struct textblk {
	char *buf;  int len, cap;
	int  *off;  int nlines, off_cap;   /* off[i] = start of line i in buf */
	int   line_open;                   /* a line has been started since the last '\n' */
};

static struct textblk dt_dom;       /* Elements view text                          */
static int  *dt_dom_node;           /* per-line: the pre-order index of its node   */
static int   dt_dom_node_cap;
static struct textblk dt_sty;       /* Styles view text                            */
static int   dt_sel = -1;           /* selected node's pre-order index (-1 = none)  */

static void tb_reset(struct textblk *t)
{
	t->len = 0; t->nlines = 0; t->line_open = 0;
}

static void tb_putc(struct textblk *t, char c)
{
	if (!t->line_open) {                            /* opening a fresh line */
		if (t->nlines >= t->off_cap) {
			int nc = t->off_cap ? t->off_cap * 2 : 64;
			int *p = realloc(t->off, (unsigned)nc * sizeof *p);
			if (!p) return;
			t->off = p; t->off_cap = nc;
		}
		t->off[t->nlines++] = t->len;
		t->line_open = 1;
	}
	if (t->len + 1 >= t->cap) {
		int nc = t->cap ? t->cap * 2 : 1024;
		char *p = realloc(t->buf, (unsigned)nc);
		if (!p) return;
		t->buf = p; t->cap = nc;
	}
	/* Terminate the line AND consume the NUL's slot, so the next line starts past
	 * it — otherwise the next char overwrites the terminator and tb_line() reads
	 * one run spanning every following line. */
	if (c == '\n') { t->buf[t->len++] = '\0'; t->line_open = 0; return; }
	t->buf[t->len++] = c;
}

static void tb_puts(struct textblk *t, const char *s) { while (*s) tb_putc(t, *s++); }
static void tb_newline(struct textblk *t) { tb_putc(t, '\n'); }
static void tb_putu(struct textblk *t, int v)
{
	char b[12]; int n = 0;
	if (v < 0) { tb_putc(t, '-'); v = -v; }
	if (v == 0) { tb_putc(t, '0'); return; }
	while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) tb_putc(t, b[--n]);
}
static void tb_puthex2(struct textblk *t, unsigned v)
{
	const char *h = "0123456789abcdef";
	tb_putc(t, h[(v >> 4) & 0xf]); tb_putc(t, h[v & 0xf]);
}
static void tb_putcolor(struct textblk *t, unsigned rgb)
{
	tb_putc(t, '#');
	tb_puthex2(t, (rgb >> 16) & 0xff);
	tb_puthex2(t, (rgb >> 8) & 0xff);
	tb_puthex2(t, rgb & 0xff);
}

/* Copy line `i` of `t` (truncated to `max` bytes) into `out`. Returns length. */
static int tb_line(const struct textblk *t, int i, char *out, int max)
{
	if (i < 0 || i >= t->nlines) { out[0] = '\0'; return 0; }
	const char *s = t->buf + t->off[i];
	int n = 0;
	while (s[n] && n < max - 1) { out[n] = s[n]; n++; }
	out[n] = '\0';
	return n;
}

/* --- console ring ---------------------------------------------------------- *
 * A fixed circular buffer of lines, each a heap string + a level. The worker
 * appends; the main thread reads under dt_lock. A pending accumulator collects
 * bytes between newlines (console.log sends a whole line, but write() chunks are
 * not guaranteed to land on a boundary). */
#define CON_MAX 400
struct conline { char *text; unsigned char level; };
static struct conline con[CON_MAX];
static int con_start, con_count;
static char *con_pend; static int con_pend_len, con_pend_cap;
static int con_pend_level;
static int con_follow = 1;          /* keep the view pinned to the newest line      */

static void con_push(int level, const char *s, int len)
{
	char *dup = malloc((unsigned)len + 1);
	if (!dup) return;
	memcpy(dup, s, (unsigned)len); dup[len] = '\0';
	int slot;
	if (con_count < CON_MAX) {
		slot = (con_start + con_count) % CON_MAX;
		con_count++;
	} else {                                        /* full: drop the oldest */
		slot = con_start;
		free(con[slot].text);
		con_start = (con_start + 1) % CON_MAX;
	}
	con[slot].text = dup; con[slot].level = (unsigned char)level;
}

void devtools_console_append(int level, const char *s, int len)
{
	if (!dt_open) return;                            /* don't capture while the panel is closed */
	dt_lock();
	for (int i = 0; i < len; i++) {
		char c = s[i];
		if (c == '\n') {
			con_push(con_pend_level, con_pend ? con_pend : "", con_pend_len);
			con_pend_len = 0;
		} else {
			if (con_pend_len + 1 >= con_pend_cap) {
				int nc = con_pend_cap ? con_pend_cap * 2 : 128;
				char *p = realloc(con_pend, (unsigned)nc);
				if (!p) break;
				con_pend = p; con_pend_cap = nc;
			}
			con_pend[con_pend_len++] = c;
			con_pend_level = level;                 /* the level of the line being built */
		}
	}
	con_follow = 1;                                 /* a new line: jump to the bottom    */
	dt_unlock();
	if (dt_open) B->seq++;                           /* repaint the console panel         */
}

/* --- network ring ---------------------------------------------------------- *
 * One preformatted line per HTTP response the page made (logged from the fetch
 * paths on the worker / image-fetch threads), reset on each top-level navigation
 * (devtools_net_reset). Same circular-buffer shape as the console. */
#define NET_MAX 256
static struct conline net[NET_MAX];
static int net_start, net_count;

static void net_push(int level, const char *s, int len)
{
	char *dup = malloc((unsigned)len + 1);
	if (!dup) return;
	memcpy(dup, s, (unsigned)len); dup[len] = '\0';
	int slot;
	if (net_count < NET_MAX) { slot = (net_start + net_count) % NET_MAX; net_count++; }
	else { slot = net_start; free(net[slot].text); net_start = (net_start + 1) % NET_MAX; }
	net[slot].text = dup; net[slot].level = (unsigned char)level;
}

void devtools_net_reset(void)
{
	dt_lock();
	for (int i = 0; i < net_count; i++) { int s = (net_start + i) % NET_MAX; free(net[s].text); net[s].text = 0; }
	net_start = 0; net_count = 0;
	dt_unlock();
	if (dt_open) B->seq++;
}

/* Append a human-readable byte size: "923 B" / "12.3 KB" / "4.1 MB". */
static void net_putsize(char *b, int *n, int cap, int size)
{
	if (size < 1024) { uapp(b, n, cap, (unsigned)(size < 0 ? 0 : size)); sapp(b, n, cap, " B"); return; }
	int unit = size, div = 1024; const char *suf = "KB";
	if (size >= 1024 * 1024) { div = 1024 * 1024; suf = "MB"; }
	uapp(b, n, cap, (unsigned)(unit / div));
	sapp(b, n, cap, ".");
	uapp(b, n, cap, (unsigned)((unit % div) * 10 / div));   /* one decimal */
	sapp(b, n, cap, " "); sapp(b, n, cap, suf);
}

void devtools_net_add(const char *method, const char *url, int status, const char *ctype, int size)
{
	char buf[URL_MAX + 200]; int n = 0;
	if (status > 0) uapp(buf, &n, sizeof buf, (unsigned)status);
	else            sapp(buf, &n, sizeof buf, "ERR");
	sapp(buf, &n, sizeof buf, "  ");
	sapp(buf, &n, sizeof buf, method ? method : "GET");
	sapp(buf, &n, sizeof buf, "  ");
	net_putsize(buf, &n, sizeof buf, size);
	sapp(buf, &n, sizeof buf, "  ");
	sapp(buf, &n, sizeof buf, url ? url : "");
	if (ctype && ctype[0]) { sapp(buf, &n, sizeof buf, "  ["); sapp(buf, &n, sizeof buf, ctype); sapp(buf, &n, sizeof buf, "]"); }

	if (!dt_open) return;                            /* don't capture while the panel is closed */
	int level = (status == 0 || status >= 400) ? DT_ERROR
	          : (status >= 300)                ? DT_WARN : DT_LOG;
	dt_lock();
	net_push(level, buf, n);
	dt_unlock();
	if (dt_open) B->seq++;
}

/* --- per-tab view state (main thread, on tab switch) ---------------------- */

/* Drop the current page's capture. Caller holds dt_lock (the rings + snapshots
 * are shared with the worker/render threads). */
static void dt_reset_content(void)
{
	for (int i = 0; i < con_count; i++) { int s = (con_start + i) % CON_MAX; free(con[s].text); con[s].text = 0; }
	con_start = con_count = 0;
	free(con_pend); con_pend = 0; con_pend_len = 0; con_pend_cap = 0;
	for (int i = 0; i < net_count; i++) { int s = (net_start + i) % NET_MAX; free(net[s].text); net[s].text = 0; }
	net_start = net_count = 0;
	tb_reset(&dt_dom); tb_reset(&dt_sty);
}

void devtools_view_save(struct dt_view *v)
{
	v->open = dt_open;
	v->tab  = dt_tab;
	for (int i = 0; i < N_TABS; i++) v->scroll[i] = dt_scroll[i];
}

void devtools_view_load(const struct dt_view *v)
{
	dt_open = v->open;
	dt_tab  = v->tab;
	for (int i = 0; i < N_TABS; i++) dt_scroll[i] = v->scroll[i];
	dt_lock();
	dt_reset_content();          /* the live page changed under us; selection is moot */
	dt_sel = -1;
	dt_dirty = 1;                /* worker rebuilds the snapshot for the new page      */
	dt_unlock();
	B->seq++;
}

/* --- DOM snapshot (worker thread) ----------------------------------------- */

/* Append ` name="value"` for each attribute, values truncated so one element
 * line stays readable. */
static void dom_emit_attrs(struct textblk *t, struct dom_node *el)
{
	for (struct dom_attr *a = el->attrs; a; a = a->next) {
		tb_putc(t, ' ');
		tb_puts(t, a->name);
		if (a->value && a->value[0]) {
			tb_puts(t, "=\"");
			int n = 0;
			for (const char *v = a->value; *v && n < 40; v++, n++) {
				char c = *v;
				tb_putc(t, (c == '\n' || c == '\t') ? ' ' : c);
			}
			if (a->value[n]) tb_puts(t, "...");
			tb_putc(t, '"');
		}
	}
}

/* Record the next-emitted line's owning node index in dt_dom_node (a closing-tag
 * line reuses the element's own index, so a click anywhere on the element — open
 * or close line — selects it). Call right before the line's first character, when
 * dt_dom.nlines still equals that line's number. */
static void dom_record(int my)
{
	if (dt_dom.nlines >= dt_dom_node_cap) {
		int nc = dt_dom_node_cap ? dt_dom_node_cap * 2 : 64;
		int *p = realloc(dt_dom_node, (unsigned)nc * sizeof *p);
		if (p) { dt_dom_node = p; dt_dom_node_cap = nc; }
	}
	if (dt_dom.nlines < dt_dom_node_cap) dt_dom_node[dt_dom.nlines] = my;
}

static void dom_indent(struct textblk *t, int depth)
{
	for (int d = 0; d < depth && d < 16; d++) tb_puts(t, "  ");
}

/* Collapsed text content (newlines/tabs → single spaces, truncated). */
static void dom_emit_text(struct textblk *t, const char *s, int maxc)
{
	int cnt = 0;
	for (; s && *s && cnt < maxc; s++) {
		char c = *s;
		if (c == '\n' || c == '\t' || c == '\r') {
			if (cnt && t->buf[t->len - 1] == ' ') continue;   /* collapse runs */
			c = ' ';
		}
		tb_putc(t, c); cnt++;
	}
	if (s && *s) tb_puts(t, "...");
}

/* Void/self-closing elements that never get a closing tag in the tree. */
static int dom_is_void(const char *tag)
{
	static const char *const v[] = { "area", "base", "br", "col", "embed", "hr",
		"img", "input", "link", "meta", "param", "source", "track", "wbr", 0 };
	for (int i = 0; v[i]; i++) if (streq(tag, v[i])) return 1;
	return 0;
}

/* True if `n`'s only child is a single (non-whitespace) text node — rendered
 * inline as `<tag>text</tag>` on one line, like a real devtools tree. */
static struct dom_node *dom_only_text(struct dom_node *n)
{
	struct dom_node *c = n->first_child;
	if (!c || c->next || c->type != DOM_TEXT) return 0;
	const char *s = c->text;
	while (s && *s && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')) s++;
	return (s && *s) ? c : 0;
}

/* Emit the DOM subtree rooted at `n`, indented by depth, with proper open/close
 * tags. Records each line's pre-order node index in dt_dom_node so a click maps a
 * row back to a node. Pure-whitespace text nodes are skipped, but `*idx` still
 * counts every node so the index space matches dom_node_at()'s pre-order walk. */
/* Upper bounds on the snapshot walk: the tree builder caps depth at 256, but a
 * page that exhausts the 64 MiB window can leave a malformed tree, so cap here
 * too rather than recurse into garbage. The index keeps advancing past the cap
 * so click-to-node mapping stays aligned with dom_node_at()'s pre-order walk. */
#define DT_DOM_MAX_DEPTH 300
#define DT_DOM_MAX_NODES 50000

static void dom_emit(struct textblk *t, struct dom_node *n, int depth, int *idx)
{
	int my = (*idx)++;
	if (!n || depth > DT_DOM_MAX_DEPTH || my > DT_DOM_MAX_NODES) return;

	if (n->type == DOM_TEXT) {
		const char *s = n->text;
		while (s && *s && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')) s++;
		if (s && *s) {
			dom_record(my);
			dom_indent(t, depth);
			tb_putc(t, '"'); dom_emit_text(t, n->text, 80); tb_putc(t, '"');
			tb_newline(t);
		}
		return;
	}

	const char *tag = n->tag[0] ? n->tag : "?";

	/* Opening tag (counts children's indices via the recursion / inline child). */
	dom_record(my);
	dom_indent(t, depth);
	tb_putc(t, '<'); tb_puts(t, tag); dom_emit_attrs(t, n);
	tb_putc(t, '>');

	if (dom_is_void(tag)) {                            /* <br>, <img> — no close */
		tb_newline(t);
		for (struct dom_node *c = n->first_child; c; c = c->next)   /* normally none */
			dom_emit(t, c, depth + 1, idx);
		return;
	}
	if (!n->first_child) {                            /* <tag></tag> on one line */
		tb_puts(t, "</"); tb_puts(t, tag); tb_putc(t, '>');
		tb_newline(t);
		return;
	}

	struct dom_node *only = dom_only_text(n);
	if (only) {                                        /* <tag>text</tag> inline */
		dom_emit_text(t, only->text, 80);
		tb_puts(t, "</"); tb_puts(t, tag); tb_putc(t, '>');
		tb_newline(t);
		(*idx)++;                                      /* the inlined text node  */
		return;
	}

	tb_newline(t);                                     /* open tag on its own line */
	for (struct dom_node *c = n->first_child; c; c = c->next)
		dom_emit(t, c, depth + 1, idx);
	dom_record(my);                                    /* closing line = same node */
	dom_indent(t, depth);
	tb_puts(t, "</"); tb_puts(t, tag); tb_putc(t, '>');
	tb_newline(t);
}

/* The node at pre-order index `target` under `n`; *idx is the running counter. */
static struct dom_node *dom_node_at(struct dom_node *n, int target, int *idx)
{
	if ((*idx)++ == target) return n;
	for (struct dom_node *c = n->first_child; c; c = c->next) {
		struct dom_node *r = dom_node_at(c, target, idx);
		if (r) return r;
	}
	return 0;
}

/* Emit the css.c-modeled declarations of one css_style, each on its own line at
 * indent `ind`. Returns the number printed (0 = the rule set nothing we model). */
static int emit_decls(const struct css_style *cs, const char *ind)
{
	int n = 0;
	#define DL(...) do { tb_puts(&dt_sty, ind); __VA_ARGS__; tb_newline(&dt_sty); n++; } while (0)
	if (cs->disp == CSS_DISP_BLOCK)  DL(tb_puts(&dt_sty, "display: block"));
	else if (cs->disp == CSS_DISP_INLINE) DL(tb_puts(&dt_sty, "display: inline"));
	else if (cs->disp == CSS_DISP_NONE)   DL(tb_puts(&dt_sty, "display: none"));
	if (cs->color >= 0) DL(tb_puts(&dt_sty, "color: "); tb_putcolor(&dt_sty, (unsigned)cs->color));
	if (cs->bg >= 0)    DL(tb_puts(&dt_sty, "background: "); tb_putcolor(&dt_sty, (unsigned)cs->bg));
	if (cs->bold == 1)  DL(tb_puts(&dt_sty, "font-weight: bold"));
	if (cs->under == 1) DL(tb_puts(&dt_sty, "text-decoration: underline"));
	if (cs->center == 1)DL(tb_puts(&dt_sty, "text-align: center"));
	if (cs->width >= 0) DL(tb_puts(&dt_sty, "width: "); tb_putu(&dt_sty, cs->width); tb_puts(&dt_sty, "px"));
	if (cs->height >= 0)DL(tb_puts(&dt_sty, "height: "); tb_putu(&dt_sty, cs->height); tb_puts(&dt_sty, "px"));
	if (cs->fam[0])     DL(tb_puts(&dt_sty, "font-family: "); tb_puts(&dt_sty, cs->fam));
	#undef DL
	return n;
}

/* Build the Styles view for the selected node, like a real devtools pane: the
 * inline style="" block, then every matching rule (winner first) with its source
 * — the selector and, when the rule lives under an @media, that prelude — so it's
 * clear which class / media query contributed. css.c models a subset of CSS, so a
 * rule that set only unmodeled properties shows an explanatory note. */
static void style_emit(struct dom_node *root)
{
	tb_reset(&dt_sty);
	if (!root) { tb_puts(&dt_sty, "(no document loaded)"); tb_newline(&dt_sty); return; }
	if (dt_sel < 0) {
		tb_puts(&dt_sty, "Select an element in the Elements tab.");
		tb_newline(&dt_sty);
		return;
	}
	int idx = 0;
	struct dom_node *n = dom_node_at(root, dt_sel, &idx);
	if (!n) { tb_puts(&dt_sty, "(selection no longer in the tree)"); tb_newline(&dt_sty); return; }
	if (n->type == DOM_TEXT) { tb_puts(&dt_sty, "(text node — no styles)"); tb_newline(&dt_sty); return; }

	tb_putc(&dt_sty, '<'); tb_puts(&dt_sty, n->tag);
	const char *id = dom_get_attr(n, "id");
	if (id && id[0]) { tb_putc(&dt_sty, '#'); tb_puts(&dt_sty, id); }
	const char *cls = dom_get_attr(n, "class");
	if (cls && cls[0]) {
		for (const char *p = cls; *p; ) {
			while (*p == ' ') p++;
			if (!*p) break;
			tb_putc(&dt_sty, '.');
			while (*p && *p != ' ') tb_putc(&dt_sty, *p++);
		}
	}
	tb_putc(&dt_sty, '>'); tb_newline(&dt_sty);
	tb_newline(&dt_sty);

	/* element.style {} — the inline style="" attribute, verbatim. */
	const char *inl = dom_get_attr(n, "style");
	if (inl && inl[0]) {
		tb_puts(&dt_sty, "element.style {"); tb_newline(&dt_sty);
		tb_puts(&dt_sty, "  ");
		for (const char *p = inl; *p; p++) {        /* one declaration per line */
			tb_putc(&dt_sty, *p);
			if (*p == ';' && p[1]) { tb_newline(&dt_sty); tb_puts(&dt_sty, "  "); }
		}
		tb_newline(&dt_sty);
		tb_puts(&dt_sty, "}"); tb_newline(&dt_sty);
		tb_newline(&dt_sty);
	}

	/* Matching rules, winner first, grouped under their @media when any. */
	struct css_match m[64];
	int nm = html_match_styles(n, m, 64);
	for (int i = 0; i < nm; i++) {
		const char *base = "";
		if (m[i].media[0]) {
			tb_puts(&dt_sty, m[i].media);
			if (!m[i].active) tb_puts(&dt_sty, "   /* not currently matched */");
			tb_puts(&dt_sty, " {"); tb_newline(&dt_sty);
			base = "  ";
		}
		tb_puts(&dt_sty, base);
		tb_puts(&dt_sty, m[i].selector); tb_puts(&dt_sty, " {"); tb_newline(&dt_sty);

		char ind[8]; int k = 0; while (base[k]) { ind[k] = base[k]; k++; } ind[k++] = ' '; ind[k++] = ' '; ind[k] = '\0';
		if (emit_decls(&m[i].decl, ind) == 0) {
			tb_puts(&dt_sty, ind);
			tb_puts(&dt_sty, "/* only properties this engine doesn't model */");
			tb_newline(&dt_sty);
		}
		tb_puts(&dt_sty, base); tb_puts(&dt_sty, "}"); tb_newline(&dt_sty);
		if (m[i].media[0]) { tb_puts(&dt_sty, "}"); tb_newline(&dt_sty); }
	}
	if (nm == 0 && !(inl && inl[0])) {
		tb_puts(&dt_sty, "No matching CSS rules.");
		tb_newline(&dt_sty);
	}
}

void devtools_worker_tick(void)
{
	if (!dt_open) return;
	if (!dt_dirty && B->seq == dt_seen_seq) return;

	struct dom_node *root = html_dom_root();
	dt_lock();
	tb_reset(&dt_dom);
	if (root) { int idx = 0; dom_emit(&dt_dom, root, 0, &idx); }
	else { tb_puts(&dt_dom, "(no document loaded)"); tb_newline(&dt_dom); }
	if (dt_sel >= dt_dom_node_cap) dt_sel = -1;
	style_emit(root);
	dt_dirty = 0;
	dt_unlock();

	B->seq++;                       /* repaint with the refreshed snapshot */
	dt_seen_seq = B->seq;           /* set AFTER the bump so the tick settles */
}

/* --- main-thread layout, render, input ------------------------------------ */

void devtools_toggle(void)
{
	dt_open = !dt_open;
	dt_dirty = 1;
	B->seq++;
}

int devtools_visible(void) { return dt_open; }

int devtools_top(void)
{
	if (!dt_open) return BH;
	int pane_top = MENU_H + TAB_H + ADDR_H;
	int h = BH * 5 / 12;
	if (h < 140) h = 140;
	int top = BH - h;
	if (top < pane_top + 24) top = pane_top + 24;   /* always leave some page visible */
	if (top < 0) top = 0;
	return top;
}

static const char *const tab_names[N_TABS] = { "Elements", "Styles", "Console", "Network" };

/* The header is a row of buttons (the shared gfx button widget): one per view,
 * the active one drawn pressed, plus a trailing close button. Geometry is shared
 * by render and the click handler so the drawn and hit rects never drift. */
#define DT_CLOSE_W 24
static int tab_x(int i)
{
	int x = 0;
	for (int k = 0; k < i; k++) x += gfx_ui_width(tab_names[k]) + 18;
	return x;
}
static int tab_w(int i) { return gfx_ui_width(tab_names[i]) + 18; }

static int content_top(int top) { return top + DT_HDR; }
static int content_rows(int top) { int h = BH - content_top(top); return h > 0 ? h / ROW_H : 0; }
static int content_w(void) { return BW - SCROLL_W; }   /* leaves room for the scrollbar */

/* Total scrollable line count of the active tab (read under dt_lock by caller). */
static int active_lines(void)
{
	if (dt_tab == TAB_ELEMENTS) return dt_dom.nlines;
	if (dt_tab == TAB_STYLES)   return dt_sty.nlines;
	if (dt_tab == TAB_NETWORK)  return net_count;
	return con_count;
}

static int clamp_scroll(int s, int total, int rows)
{
	int max = total - rows; if (max < 0) max = 0;
	if (s > max) s = max;
	if (s < 0) s = 0;
	return s;
}

/* Syntax-highlight palette (dark theme, à la a real devtools markup view). */
#define DT_C_TEXT    0xcdd3de
#define DT_C_PUNCT   0x9aa0a6   /* < > / = and whitespace             */
#define DT_C_TAG     0x7cb7e8   /* element tag name                   */
#define DT_C_ATTR    0xe0a060   /* attribute name                     */
#define DT_C_VALUE   0x9bce8a   /* attribute value (incl. its quotes) */

/* Draw a run [run,run+rl) at x in colour col with the monospace UTF-8 font,
 * returning the advanced x (one cell per codepoint) so colour segments abut. */
static int flush_run(int x, int y, char *run, int *rl, unsigned col)
{
	if (*rl <= 0) return x;
	run[*rl] = '\0';
	x = gfx_text_utf8(x, y, run, col, GFX_TRANSPARENT);
	*rl = 0;
	return x;
}

/* Render one DOM-markup line with token colours: brackets/`=` punct, tag names,
 * attribute names, quoted values, and text content. A tiny scanner — `in_tag`
 * tracks `<…>`, `in_quote` a value, `tag_name` the first identifier after `<`. */
static void draw_dom_line(int x, int y, const char *s)
{
	char run[260]; int rl = 0; unsigned cur = DT_C_TEXT;
	int in_tag = 0, in_quote = 0, tag_name = 0;
	for (int i = 0; s[i]; i++) {
		char c = s[i];
		unsigned col;
		if (in_quote) { col = DT_C_VALUE; if (c == '"') in_quote = 0; }
		else if (c == '<') { in_tag = 1; tag_name = 1; col = DT_C_PUNCT; }
		else if (c == '>') { in_tag = 0; tag_name = 0; col = DT_C_PUNCT; }
		else if (in_tag) {
			if (c == '"') { in_quote = 1; col = DT_C_VALUE; }
			else if (c == '/' || c == '=') col = DT_C_PUNCT;
			else if (c == ' ') { tag_name = 0; col = DT_C_PUNCT; }
			else col = tag_name ? DT_C_TAG : DT_C_ATTR;
		} else col = DT_C_TEXT;

		if (rl && col != cur) x = flush_run(x, y, run, &rl, cur);
		cur = col;
		if (rl < (int)sizeof run - 1) run[rl++] = c;
	}
	flush_run(x, y, run, &rl, cur);
}

/* Render one Styles line: a `<tag#id.class>` header in tag colour; a
 * `name: value` declaration with the property dim and the value highlighted. */
static void draw_style_line(int x, int y, const char *s)
{
	if (s[0] == '<') { gfx_text_utf8(x, y, s, DT_C_TAG, GFX_TRANSPARENT); return; }
	int colon = -1;
	for (int i = 0; s[i]; i++) if (s[i] == ':') { colon = i; break; }
	if (colon < 0) { gfx_text_utf8(x, y, s, DT_C_TEXT, GFX_TRANSPARENT); return; }
	char run[260]; int rl = 0;
	for (int i = 0; i <= colon && rl < (int)sizeof run - 1; i++) run[rl++] = s[i];
	x = flush_run(x, y, run, &rl, DT_C_ATTR);          /* "name:"  */
	gfx_text_utf8(x, y, s + colon + 1, DT_C_VALUE, GFX_TRANSPARENT);   /* " value" */
}

void devtools_render(void)
{
	if (!dt_open) return;
	int top = devtools_top();
	int w = BW, h = BH - top;

	dt_lock();

	gfx_fill_rect(0, top, w, h, 0x1e1e1e);          /* panel body */
	gfx_fill_rect(0, top, w, DT_HDR, 0x14171c);     /* header row */

	for (int i = 0; i < N_TABS; i++)
		button_draw(tab_x(i), top, tab_w(i), DT_HDR, tab_names[i],
		            i == dt_tab ? BTN_PRESSED : BTN_NORMAL);
	button_draw(w - DT_CLOSE_W, top, DT_CLOSE_W, DT_HDR, "\xc3\x97", BTN_NORMAL);   /* × */

	int ct = content_top(top), rows = content_rows(top), cw = content_w();
	int total = active_lines();
	int *sc = &dt_scroll[dt_tab];

	if (dt_tab == TAB_CONSOLE && con_follow)
		*sc = total;                                 /* pinned to the newest line */
	*sc = clamp_scroll(*sc, total, rows);
	if (dt_tab == TAB_CONSOLE)
		con_follow = (*sc >= total - rows);          /* at bottom -> keep following */

	int max = (cw - 8) / GLYPH_W; if (max > 255) max = 255; if (max < 1) max = 1;
	char line[256];
	for (int r = 0; r < rows; r++) {
		int idx = *sc + r;
		if (idx >= total) break;
		int y = ct + r * ROW_H;
		if (dt_tab == TAB_CONSOLE || dt_tab == TAB_NETWORK) {
			struct conline *ring = dt_tab == TAB_CONSOLE ? con : net;
			int cap = dt_tab == TAB_CONSOLE ? CON_MAX : NET_MAX;
			int base = dt_tab == TAB_CONSOLE ? con_start : net_start;
			int slot = (base + idx) % cap;
			unsigned fg = ring[slot].level == DT_ERROR ? 0xf08080
			            : ring[slot].level == DT_WARN  ? 0xe0c060 : 0xcdd3de;
			if (ring[slot].level == DT_ERROR)
				gfx_fill_rect(0, y, cw, ROW_H, 0x3a2222);
			int n = 0; const char *s = ring[slot].text;
			while (s && s[n] && n < max - 1) { line[n] = s[n]; n++; } line[n] = '\0';
			gfx_text_utf8(6, y, line, fg, GFX_TRANSPARENT);
		} else if (dt_tab == TAB_ELEMENTS) {
			int sel = (idx < dt_dom_node_cap && dt_dom_node[idx] == dt_sel);
			if (sel) gfx_fill_rect(0, y, cw, ROW_H, 0x094771);
			tb_line(&dt_dom, idx, line, max);
			draw_dom_line(6, y, line);
		} else {
			tb_line(&dt_sty, idx, line, max);
			draw_style_line(6, y, line);
		}
	}

	scrollbar_draw(w - SCROLL_W, ct, SCROLL_W, BH - ct, *sc, total, rows);
	dt_unlock();
}

int devtools_handle_click(int mx, int my)
{
	if (!dt_open) return 0;
	int top = devtools_top();
	if (my < top) return 0;                          /* above the panel: not ours */

	if (my < top + DT_HDR) {                          /* header buttons */
		if (button_hit(BW - DT_CLOSE_W, top, DT_CLOSE_W, DT_HDR, mx, my)) {
			devtools_toggle(); return 1;
		}
		for (int i = 0; i < N_TABS; i++)
			if (button_hit(tab_x(i), top, tab_w(i), DT_HDR, mx, my)) {
				dt_tab = i; B->seq++; return 1;
			}
		return 1;
	}

	int ct = content_top(top), rows = content_rows(top);
	dt_lock();
	int total = active_lines();
	dt_unlock();

	int part = scrollbar_hit(BW - SCROLL_W, ct, SCROLL_W, BH - ct, mx, my);
	if (part != SCROLLBAR_NONE) {                    /* scrollbar (reused widget) */
		if (part == SCROLLBAR_UP)        dt_scroll[dt_tab] -= 1;
		else if (part == SCROLLBAR_DOWN) dt_scroll[dt_tab] += 1;
		else dt_scroll[dt_tab] = scrollbar_pos(BW - SCROLL_W, ct, SCROLL_W, BH - ct,
		                                       mx, my, dt_scroll[dt_tab], total, rows);
		dt_scroll[dt_tab] = clamp_scroll(dt_scroll[dt_tab], total, rows);
		if (dt_tab == TAB_CONSOLE) con_follow = (dt_scroll[dt_tab] >= total - rows);
		B->seq++;
		return 1;
	}

	if (mx < content_w() && dt_tab == TAB_ELEMENTS) {   /* select a DOM node */
		int row = dt_scroll[dt_tab] + (my - ct) / ROW_H;
		dt_lock();
		int ok = row >= 0 && row < dt_dom.nlines && row < dt_dom_node_cap;
		int node = ok ? dt_dom_node[row] : -1;
		dt_unlock();
		if (ok) { dt_sel = node; dt_dirty = 1; B->seq++; }
	}
	return 1;
}

int devtools_handle_scroll(int delta)
{
	if (!dt_open) return 0;
	int top = devtools_top();
	int rows = content_rows(top);
	dt_lock();
	int total = active_lines();
	dt_unlock();
	int prev = dt_scroll[dt_tab];
	int next = clamp_scroll(prev - delta * 3, total, rows);
	if (dt_tab == TAB_CONSOLE)
		con_follow = (next >= total - rows);
	if (next != prev) { dt_scroll[dt_tab] = next; B->seq++; }  /* repaint only on real move */
	return 1;
}

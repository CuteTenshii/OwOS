#include "clipboard.h"
#include "html_int.h"
#include "mime.h"

/* The focused form control (a text input or textarea), or NULL */
struct dom_node *g_focus;
static bool g_sel_all; /* the focused field's whole value is selected (Ctrl+A) */
struct dom_node *html_focus(void) { return g_focus; }

/* Set by the control emitters during the walk */
bool g_has_controls;
int html_has_controls(void) { return g_has_controls; }

static bool input_is_text(const char *type)
{
  if (!type || !type[0])
    return true;
  return streq(type, "text") || streq(type, "search") || streq(type, "email") || streq(type, "url") ||
         streq(type, "tel") || streq(type, "password") || streq(type, "number");
}

int html_control_kind(struct dom_node *n)
{
  if (!n || n->type != DOM_ELEMENT)
    return CTRL_NONE;
  if (streq(n->tag, "textarea"))
    return CTRL_TEXTAREA;
  if (streq(n->tag, "select"))
    return CTRL_SELECT;
  if (streq(n->tag, "button")) {
    const char *t = dom_get_attr(n, "type");
    /* A <button> defaults to type=submit; only explicit button/reset don't. */
    return (t && (streq(t, "button") || streq(t, "reset"))) ? CTRL_BUTTON : CTRL_SUBMIT;
  }
  if (streq(n->tag, "input")) {
    const char *t = dom_get_attr(n, "type");
    if (t && streq(t, "checkbox"))
      return CTRL_CHECKBOX;
    if (t && streq(t, "radio"))
      return CTRL_RADIO;
    if (t && (streq(t, "submit") || streq(t, "button") || streq(t, "reset")))
      return CTRL_SUBMIT;
    if (t && streq(t, "hidden"))
      return CTRL_NONE;
    if (t && streq(t, "file"))
      return CTRL_FILE;
    return CTRL_TEXT;
  }
  return CTRL_NONE;
}

void html_set_focus(struct dom_node *n)
{
  int k = html_control_kind(n);
  g_focus = (k == CTRL_TEXT || k == CTRL_TEXTAREA) ? n : 0;
  g_sel_all = false;
  B->focus_kind = (k == CTRL_TEXT) ? 1 : (k == CTRL_TEXTAREA) ? 2 : 0;
}

/* --- file inputs --- */

static const char *base_name(const char *path)
{
  const char *b = path;
  for (const char *q = path; *q; q++)
    if (*q == '/')
      b = q + 1;
  return b;
}

struct file_list {
  struct dom_files *f;
  int cap;
};

#define PICK_FILES_MAX 5000

static void fl_add(struct file_list *l, const char *path, const char *rel)
{
  struct dom_files *f = l->f;
  if (f->n >= PICK_FILES_MAX)
    return;
  if (f->n == l->cap) {
    int cap = l->cap ? 2 * l->cap : 32;
    char **p = realloc(f->path, (unsigned)cap * sizeof *p);
    if (!p)
      return;
    f->path = p;
    char **r = realloc(f->rel, (unsigned)cap * sizeof *r);
    if (!r)
      return;
    f->rel = r;
    l->cap = cap;
  }
  f->path[f->n] = strdup(path);
  f->rel[f->n] = strdup(rel);
  f->n++;
}

/* Every file under dir; rel is dir's path relative to the picked folder's parent */
static void walk_dir(struct file_list *l, const char *dir, const char *rel)
{
  struct dirent_u d;
  for (int i = 0; readdir(dir, i, &d) == 1; i++) {
    char path[512], r[512];
    snprintf(path, sizeof path, "%s/%s", dir, d.name);
    snprintf(r, sizeof r, rel[0] ? "%s/%s" : "%s%s", rel, d.name);
    if (d.is_dir)
      walk_dir(l, path, r);
    else
      fl_add(l, path, r);
  }
}

int html_file_choose(struct dom_node *in)
{
  int flags = 0;
  if (dom_get_attr(in, "webkitdirectory") || dom_get_attr(in, "directory"))
    flags = PICK_DIRECTORY;
  else if (dom_get_attr(in, "multiple"))
    flags = PICK_MULTIPLE;
  char *paths = malloc(sizeof B->dlg_paths);
  struct dom_files *sel = calloc(1, sizeof *sel);
  int n = paths && sel ? worker_pick(flags, dom_get_attr(in, "accept"), paths, sizeof B->dlg_paths) : 0;
  if (n <= 0) {
    free(paths);
    free(sel);
    return 0;
  }
  struct file_list l = {sel, 0};
  if (flags & PICK_DIRECTORY) {
    char *dir = paths;
    for (int k = (int)strlen(dir); k > 1 && dir[k - 1] == '/'; k--)
      dir[k - 1] = 0;
    bool root = streq(dir, "/");
    walk_dir(&l, root ? "" : dir, root ? "" : base_name(dir));
  } else {
    for (const char *p = paths; *p && sel->n < n; p += strlen(p) + 1)
      fl_add(&l, p, "");
  }
  free(paths);
  dom_files_free(in->files);
  in->files = sel;
  dom_mutation_seq++;
  return 1;
}

/* What the control shows next to its button */
static void file_label(const struct dom_node *in, char *out, int cap)
{
  const struct dom_files *f = in->files;
  if (!f || !f->n)
    snprintf(out, cap, "No file chosen");
  else if (f->n == 1 && !f->rel[0][0])
    snprintf(out, cap, "%s", base_name(f->path[0]));
  else
    snprintf(out, cap, "%d files", f->n);
}

/* --- control default actions (worker-side */

void html_toggle_checked(struct dom_node *c)
{
  if (dom_get_attr(c, "checked"))
    dom_remove_attr(c, "checked");
  else
    dom_set_attr(c, "checked", "");
}

static struct dom_node *form_of(struct dom_node *n)
{
  for (struct dom_node *p = n; p; p = p->parent)
    if (streq(p->tag, "form"))
      return p;
  return 0;
}
static void radio_clear(struct dom_node *root, const char *name, struct dom_node *keep)
{
  for (struct dom_node *c = root->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    if (streq(c->tag, "input")) {
      const char *t = dom_get_attr(c, "type"), *nm = dom_get_attr(c, "name");
      if (t && streq(t, "radio") && c != keep && nm && name && streq(nm, name))
        dom_remove_attr(c, "checked");
    }
    radio_clear(c, name, keep);
  }
}
void html_radio_select(struct dom_node *c)
{
  const char *name = dom_get_attr(c, "name");
  struct dom_node *scope = form_of(c);
  if (!scope)
    scope = g_live_dom;
  if (name && scope)
    radio_clear(scope, name, c);
  dom_set_attr(c, "checked", "");
}

void html_select_cycle(struct dom_node *sel)
{
  struct dom_node *first = 0, *cur = 0, *nextopt = 0;
  bool take = false;
  for (struct dom_node *o = sel->first_child; o; o = o->next) {
    if (o->type != DOM_ELEMENT || !streq(o->tag, "option"))
      continue;
    if (!first)
      first = o;
    if (take) {
      nextopt = o;
      take = false;
    }
    if (dom_get_attr(o, "selected")) {
      cur = o;
      take = true;
    }
  }
  if (!cur) {
    if (first)
      dom_set_attr(first, "selected", "");
    return;
  }
  dom_remove_attr(cur, "selected");
  dom_set_attr(nextopt ? nextopt : first, "selected", ""); /* wrap to first */
}

void html_seed_textarea(struct dom_node *ta)
{
  char buf[1024];
  int n = 0;
  for (struct dom_node *c = ta->first_child; c; c = c->next)
    if (c->type == DOM_TEXT && c->text)
      for (const char *p = c->text; *p && n < (int)sizeof buf - 1; p++)
        buf[n++] = *p;
  buf[n] = '\0';
  dom_set_attr(ta, "value", buf);
}

/* Edit the focused text control's value */
struct dom_node *html_focus_edit(int ch)
{
  char buf[2048];
  int n = 0;
  const char *v;
  if (!g_focus || (ch == KEY_DELETE && !g_sel_all))
    return 0;
  v = dom_get_attr(g_focus, "value");
  if (v && !g_sel_all)
    for (const char *p = v; *p && n < (int)sizeof buf - 1; p++)
      buf[n++] = *p;
  if (ch == '\b' || ch == 127) {
    if (n > 0)
      n--;
  } else if (ch == '\n' || ch == '\r') {
    if (n < (int)sizeof buf - 1)
      buf[n++] = '\n';
  } /* textarea newline */
  else if (ch >= 0x20 && n < (int)sizeof buf - 1)
    buf[n++] = (char)ch;
  buf[n] = '\0';
  g_sel_all = false;
  dom_set_attr(g_focus, "value", buf);
  return g_focus;
}

int html_focus_command(int cmd)
{
  if (!g_focus)
    return 0;
  if (cmd == EDIT_SELECT_ALL) {
    g_sel_all = true;
    return 1;
  }
  const char *v = dom_get_attr(g_focus, "value");
  if (cmd == KEY_INTR && g_sel_all && v)
    clip_put_text(v, (int)strlen(v));
  return 0;
}

/* Growable byte buffer for an encoded field set */
struct fbuf {
  char *p;
  int n, cap;
  bool oom;
};

static void fb_put(struct fbuf *b, const char *s, int n)
{
  if (b->oom || n <= 0)
    return;
  if (b->n + n + 1 > b->cap) {
    int cap = b->cap ? b->cap : 256;
    while (cap < b->n + n + 1)
      cap *= 2;
    char *g = realloc(b->p, (unsigned)cap);
    if (!g) {
      b->oom = true;
      return;
    }
    b->p = g;
    b->cap = cap;
  }
  memcpy(b->p + b->n, s, (unsigned)n);
  b->n += n;
  b->p[b->n] = '\0';
}
static void fb_str(struct fbuf *b, const char *s) { fb_put(b, s, (int)strlen(s)); }

/* application/x-www-form-urlencoded component encoding */
static bool form_unreserved(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '*';
}
static void enc_field(struct fbuf *b, const char *s)
{
  static const char hex[] = "0123456789ABCDEF";
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == ' ')
      fb_put(b, "+", 1);
    else if (form_unreserved(c))
      fb_put(b, (const char *)&c, 1);
    else {
      char e[3] = {'%', hex[c >> 4], hex[c & 15]};
      fb_put(b, e, 3);
    }
  }
}

/* A multipart header parameter: the value quoted, with quotes and line breaks percent-escaped */
static void fb_quoted(struct fbuf *b, const char *s)
{
  fb_put(b, "\"", 1);
  for (; s && *s; s++) {
    if (*s == '"')
      fb_str(b, "%22");
    else if (*s == '\r')
      fb_str(b, "%0D");
    else if (*s == '\n')
      fb_str(b, "%0A");
    else
      fb_put(b, s, 1);
  }
  fb_put(b, "\"", 1);
}

/* Append one "name=value" pair to the field set */
enum { FE_URLENC, FE_PLAIN, FE_MULTI };
struct fenc {
  struct fbuf b;
  int kind;
  char boundary[48];
};

static void part_head(struct fenc *f, const char *name)
{
  fb_str(&f->b, "--");
  fb_str(&f->b, f->boundary);
  fb_str(&f->b, "\r\nContent-Disposition: form-data; name=");
  fb_quoted(&f->b, name);
}

static void q_field(struct fenc *f, const char *name, const char *val)
{
  if (f->kind == FE_MULTI) {
    part_head(f, name);
    fb_str(&f->b, "\r\n\r\n");
    fb_str(&f->b, val ? val : "");
    fb_str(&f->b, "\r\n");
    return;
  }
  if (f->b.n)
    fb_put(&f->b, f->kind == FE_PLAIN ? "\n" : "&", 1);
  if (f->kind == FE_PLAIN) {
    fb_str(&f->b, name);
    fb_put(&f->b, "=", 1);
    fb_str(&f->b, val ? val : "");
  } else {
    enc_field(&f->b, name);
    fb_put(&f->b, "=", 1);
    enc_field(&f->b, val ? val : "");
  }
}

#define FILE_MAX (32 << 20) /* the largest file a submission carries */

/* The fields of an <input type=file>: one part per file (an empty one when nothing is chosen) */
static void file_fields(struct fenc *f, const char *name, const struct dom_node *in)
{
  const struct dom_files *fl = in->files;
  if (f->kind != FE_MULTI) {
    if (!fl || !fl->n)
      q_field(f, name, "");
    for (int i = 0; fl && i < fl->n; i++)
      q_field(f, name, base_name(fl->path[i]));
    return;
  }
  if (!fl || !fl->n) {
    part_head(f, name);
    fb_str(&f->b, "; filename=\"\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n");
    return;
  }
  for (int i = 0; i < fl->n; i++) {
    struct statinfo st;
    char *data = 0;
    int len = 0;
    if (stat(fl->path[i], &st) == 0 && !st.is_dir && st.size <= FILE_MAX) {
      data = malloc(st.size + 1);
      len = data ? readfile_at(fl->path[i], 0, data, st.size) : 0;
    }
    if (len < 0)
      len = 0;
    part_head(f, name);
    fb_str(&f->b, "; filename=");
    fb_quoted(&f->b, fl->rel[i][0] ? fl->rel[i] : base_name(fl->path[i]));
    fb_str(&f->b, "\r\nContent-Type: ");
    fb_str(&f->b, mime_from_ext(fl->path[i]));
    fb_str(&f->b, "\r\n\r\n");
    fb_put(&f->b, data, len);
    fb_str(&f->b, "\r\n");
    free(data);
  }
}

/* Collect the successful controls of a form into an encoded field set */
static void form_fields(struct dom_node *n, struct fenc *f, struct dom_node *clicked)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT)
      continue;
    const char *tag = c->tag, *name = dom_get_attr(c, "name");
    const char *type = dom_get_attr(c, "type");
    if (name && name[0] &&
        (streq(tag, "input") || streq(tag, "select") || streq(tag, "textarea") || streq(tag, "button"))) {
      bool is_btn = streq(tag, "button")
                        ? !(type && (streq(type, "reset") || streq(type, "button"))) /* default type=submit */
                        : (type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset")));
      if (is_btn) {
        /* Only the activated submit control is a successful field. */
        if (c == clicked && !(type && (streq(type, "button") || streq(type, "reset"))))
          q_field(f, name, dom_get_attr(c, "value"));
      } else if (type && (streq(type, "checkbox") || streq(type, "radio"))) {
        if (dom_get_attr(c, "checked")) {
          const char *v = dom_get_attr(c, "value");
          q_field(f, name, v ? v : "on");
        }
      } else if (type && streq(type, "file")) {
        file_fields(f, name, c);
      } else {
        q_field(f, name, dom_get_attr(c, "value"));
      }
    }
    form_fields(c, f, clicked);
  }
}

int html_form_build(struct dom_node *control, struct form_submission *fs)
{
  struct dom_node *form = form_of(control);
  const char *action, *method, *enctype;
  int on = 0;
  bool is_post;
  struct fenc f = {.kind = FE_URLENC};
  if (!form)
    return 0;
  fs->url[0] = '\0';
  fs->body = 0;
  fs->body_len = 0;
  fs->ctype[0] = '\0';
  strcpy(fs->method, "GET");

  action = control ? dom_get_attr(control, "formaction") : 0;
  method = control ? dom_get_attr(control, "formmethod") : 0;
  enctype = control ? dom_get_attr(control, "formenctype") : 0;
  if (!action || !action[0])
    action = dom_get_attr(form, "action");
  if (!method || !method[0])
    method = dom_get_attr(form, "method");
  if (!enctype || !enctype[0])
    enctype = dom_get_attr(form, "enctype");

  if (action && action[0])
    resolve_url(g_cur_page_url, action, fs->url, sizeof fs->url);
  else {
    sapp(fs->url, &on, sizeof fs->url, g_cur_page_url);
  }

  /* CSP form-action: refuse a submission whose target the policy forbids. Logged unconditionally */
  if (B->pg->csp) {
    char fa[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_FORM, fs->url, g_cur_page_url, fa, sizeof fa, reason, sizeof reason)) {
      char m[URL_MAX + 96];
      int n = 0;
      sapp(m, &n, sizeof m, "[CSP] refused form submission to ");
      sapp(m, &n, sizeof m, fs->url);
      sapp(m, &n, sizeof m, " (");
      sapp(m, &n, sizeof m, reason);
      sapp(m, &n, sizeof m, ")\n");
      devtools_console_append(DT_ERROR, m, n);
      return 0;
    }
    strcpy(fs->url, fa); /* use upgraded action */
  }

  is_post = method && streq_ci(method, "post");
  if (enctype && streq_ci(enctype, "text/plain"))
    f.kind = FE_PLAIN;
  else if (is_post && enctype && streq_ci(enctype, "multipart/form-data")) {
    unsigned r[2];
    rng_bytes(r, sizeof r);
    snprintf(f.boundary, sizeof f.boundary, "----OwOSFormBoundary%08x%08x", r[0], r[1]);
    f.kind = FE_MULTI;
  }

  form_fields(form, &f, control);
  if (f.kind == FE_MULTI) {
    fb_str(&f.b, "--");
    fb_str(&f.b, f.boundary);
    fb_str(&f.b, "--\r\n");
  }

  if (is_post) {
    strcpy(fs->method, "POST");
    if (!f.b.p)
      f.b.p = calloc(1, 1);
    fs->body = f.b.p;
    fs->body_len = f.b.n;
    if (f.kind == FE_MULTI)
      snprintf(fs->ctype, sizeof fs->ctype, "multipart/form-data; boundary=%s", f.boundary);
    else
      strcpy(fs->ctype, f.kind == FE_PLAIN ? "text/plain;charset=UTF-8" : "application/x-www-form-urlencoded");
  } else {
    if (f.b.n > 0) {
      on = (int)strlen(fs->url);
      sapp(fs->url, &on, sizeof fs->url, "?");
      sapp(fs->url, &on, sizeof fs->url, f.b.p);
    }
    free(f.b.p);
  }
  return 1;
}

/* Render <input> as a boxed widget reflecting its live value/checked state */
void emit_input(struct dom_node *el)
{
  const char *type = dom_get_attr(el, "type");
  const char *val = dom_get_attr(el, "value");
  const char *ph = dom_get_attr(el, "placeholder");
  if (type && streq(type, "hidden"))
    return;
  if (type && (streq(type, "submit") || streq(type, "button") || streq(type, "reset"))) {
    emit_lit("[ ", ST_BOLD);
    emit_lit(val ? val : (streq(type, "reset") ? "Reset" : "Submit"), ST_BOLD);
    emit_lit(" ]", ST_BOLD);
    return;
  }
  if (type && (streq(type, "checkbox") || streq(type, "radio"))) {
    bool on = dom_get_attr(el, "checked") != 0;
    if (streq(type, "radio"))
      emit_lit(on ? "(o)" : "( )", ST_BOLD);
    else
      emit_lit(on ? "[x]" : "[ ]", ST_BOLD);
    return;
  }
  if (type && streq(type, "file")) {
    char label[96];
    bool dir = dom_get_attr(el, "webkitdirectory") || dom_get_attr(el, "directory");
    file_label(el, label, sizeof label);
    emit_lit(dir                            ? "[ Choose Folder ] "
             : dom_get_attr(el, "multiple") ? "[ Choose Files ] "
                                            : "[ Choose File ] ",
             ST_BOLD);
    emit_lit(label, 0);
    return;
  }
  if (!input_is_text(type)) {
    emit_lit("[ ", ST_BOLD);
    emit_lit(val ? val : "", 0);
    emit_lit(" ]", ST_BOLD);
    return;
  }
  {
    bool focused = (el == g_focus);
    emit_lit("[ ", ST_BOLD);
    if (val && val[0])
      emit_lit(val, focused && g_sel_all ? ST_UNDER : 0);
    else if (!focused && ph)
      emit_lit(ph, 0);
    else if (!focused)
      emit_lit("         ", ST_BOLD);
    if (focused)
      emit_lit("|", ST_BOLD); /* caret */
    emit_lit(" ]", ST_BOLD);
  }
}

/* <textarea>: edits land in a synthetic "value" attribute (seeded from the initial text on first */
void emit_textarea(struct dom_node *el)
{
  const char *val = dom_get_attr(el, "value");
  bool focused = (el == g_focus);
  emit_lit("[ ", ST_BOLD);
  if (val)
    emit_lit(val, focused && g_sel_all ? ST_UNDER : 0);
  else
    walk(el);
  if (focused)
    emit_lit("|", ST_BOLD);
  emit_lit(" ]", ST_BOLD);
}

/* Render <select> as a boxed control showing the selected (or first) option. */
void emit_select(struct dom_node *el)
{
  struct dom_node *pick = 0;
  for (struct dom_node *o = el->first_child; o; o = o->next) {
    if (o->type == DOM_TEXT || !streq(o->tag, "option"))
      continue;
    if (!pick)
      pick = o;
    if (dom_get_attr(o, "selected")) {
      pick = o;
      break;
    }
  }
  emit_lit("[ ", ST_BOLD);
  if (pick)
    for (struct dom_node *t = pick->first_child; t; t = t->next)
      if (t->type == DOM_TEXT && t->text)
        emit_text(t->text);
  emit_lit(" \xe2\x96\xbe ]", ST_BOLD); /* small down triangle */
}

/* Parse a numeric attribute (value/min/max) into thousandths, so "0.6" -> 600 and "8" -> 8000 */
static long attr_milli(struct dom_node *el, const char *name, long def)
{
  const char *s = dom_get_attr(el, name);
  if (!s)
    return def;
  while (*s == ' ' || *s == '\t')
    s++;
  int neg = 0;
  if (*s == '+')
    s++;
  else if (*s == '-') {
    neg = 1;
    s++;
  }
  if (!((*s >= '0' && *s <= '9') || *s == '.'))
    return def;
  long ip = 0;
  while (*s >= '0' && *s <= '9')
    ip = ip * 10 + (*s++ - '0');
  long frac = 0, place = 100; /* first fractional digit is worth 100 milli */
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') {
      if (place > 0) {
        frac += (long)(*s - '0') * place;
        place /= 10;
      }
      s++;
    }
  }
  long v = ip * 1000 + frac;
  return neg ? -v : v;
}

/* Draw a fixed-width fill bar for <progress>/<meter>: fill_milli is 0..1000. */
static void emit_bar(long fill_milli)
{
  if (fill_milli < 0)
    fill_milli = 0;
  if (fill_milli > 1000)
    fill_milli = 1000;
  const int cells = 12;
  int filled = (int)((fill_milli * cells + 500) / 1000);
  emit_lit("\xe2\x96\x8f", ST_BOLD); /* ▏ left cap */
  for (int i = 0; i < cells; i++)
    emit_lit(i < filled ? "\xe2\x96\x88" /* █ full block  */
                        : "\xe2\x96\x91",
             0);                     /* ░ light shade */
  emit_lit("\xe2\x96\x95", ST_BOLD); /* ▕ right cap */
  int pct = (int)((fill_milli + 5) / 10);
  char buf[8];
  int k = 0;
  buf[k++] = ' ';
  if (pct >= 100) {
    buf[k++] = '1';
    buf[k++] = '0';
    buf[k++] = '0';
  } else {
    if (pct >= 10)
      buf[k++] = (char)('0' + pct / 10);
    buf[k++] = (char)('0' + pct % 10);
  }
  buf[k++] = '%';
  buf[k] = 0;
  emit_lit(buf, 0);
}

void emit_progress(struct dom_node *el)
{
  if (!dom_get_attr(el, "value")) { /* indeterminate: a flat striped bar */
    emit_lit("\xe2\x96\x8f", ST_BOLD);
    for (int i = 0; i < 12; i++)
      emit_lit("\xe2\x96\x92", 0); /* ▒ */
    emit_lit("\xe2\x96\x95", ST_BOLD);
    return;
  }
  long val = attr_milli(el, "value", 0);
  long max = attr_milli(el, "max", 1000);
  if (max <= 0)
    max = 1000;
  emit_bar(val * 1000 / max);
}

void emit_meter(struct dom_node *el)
{
  long val = attr_milli(el, "value", 0);
  long min = attr_milli(el, "min", 0);
  long max = attr_milli(el, "max", 1000);
  if (max <= min)
    max = min + 1000;
  emit_bar((val - min) * 1000 / (max - min));
}

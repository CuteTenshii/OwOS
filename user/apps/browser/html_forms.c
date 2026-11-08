#include "clipboard.h"
#include "html_int.h"

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

/* application/x-www-form-urlencoded component encoding */
static bool form_unreserved(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '*';
}
static void enc_field(char *q, int *qn, int cap, const char *s)
{
  static const char hex[] = "0123456789ABCDEF";
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == ' ') {
      if (*qn < cap - 1)
        q[(*qn)++] = '+';
    } else if (form_unreserved(c)) {
      if (*qn < cap - 1)
        q[(*qn)++] = (char)c;
    } else if (*qn < cap - 4) {
      q[(*qn)++] = '%';
      q[(*qn)++] = hex[c >> 4];
      q[(*qn)++] = hex[c & 15];
    }
  }
  q[*qn] = '\0';
}

/* Append one "name=value" pair to the encoded field set */
enum { FE_URLENC, FE_PLAIN };
static void q_field(char *q, int *qn, int cap, const char *name, const char *val, int enc)
{
  if (*qn && *qn < cap - 1)
    q[(*qn)++] = (enc == FE_PLAIN) ? '\n' : '&';
  if (enc == FE_PLAIN) {
    sapp(q, qn, cap, name);
    if (*qn < cap - 1)
      q[(*qn)++] = '=';
    sapp(q, qn, cap, val ? val : "");
    q[*qn] = '\0';
  } else {
    enc_field(q, qn, cap, name);
    if (*qn < cap - 1)
      q[(*qn)++] = '=';
    enc_field(q, qn, cap, val ? val : "");
  }
}

/* Collect the successful controls of a form into an encoded field set */
static void form_fields(struct dom_node *n, char *q, int *qn, int cap, struct dom_node *clicked, int enc)
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
          q_field(q, qn, cap, name, dom_get_attr(c, "value"), enc);
      } else if (type && (streq(type, "checkbox") || streq(type, "radio"))) {
        if (dom_get_attr(c, "checked")) {
          const char *v = dom_get_attr(c, "value");
          q_field(q, qn, cap, name, v ? v : "on", enc);
        }
      } else {
        q_field(q, qn, cap, name, dom_get_attr(c, "value"), enc);
      }
    }
    form_fields(c, q, qn, cap, clicked, enc);
  }
}

int html_form_build(struct dom_node *control, struct form_submission *fs)
{
  struct dom_node *form = form_of(control);
  const char *action, *method, *enctype;
  char q[POST_BODY_MAX];
  int qn = 0, on = 0;
  bool is_post;
  int enc;
  if (!form)
    return 0;
  fs->url[0] = '\0';
  fs->body[0] = '\0';
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
  /* multipart/form-data needs file uploads (no file picker here) */
  enc = (enctype && streq_ci(enctype, "text/plain")) ? FE_PLAIN : FE_URLENC;

  q[0] = '\0';
  form_fields(form, q, &qn, sizeof q, control, enc);

  if (is_post) {
    strcpy(fs->method, "POST");
    if (qn > (int)sizeof fs->body - 1)
      qn = (int)sizeof fs->body - 1;
    memcpy(fs->body, q, (unsigned)qn);
    fs->body[qn] = '\0';
    fs->body_len = qn;
    strcpy(fs->ctype, enc == FE_PLAIN ? "text/plain;charset=UTF-8" : "application/x-www-form-urlencoded");
  } else if (qn > 0) {
    on = (int)strlen(fs->url);
    sapp(fs->url, &on, sizeof fs->url, "?");
    sapp(fs->url, &on, sizeof fs->url, q);
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

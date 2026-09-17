/* lexer.c — JavaScript tokenizer. See lexer.h for the design notes. */
#include "lexer.h"
#include "fmt.h"  /* strtod (decimal forms only; hex/oct/bin done here) */
#include "owos.h" /* mem*, str*, utf8_encode, malloc/free */

/* ------------------------------------------------------- char classes ----- */

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_hex(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hexval(int c) { return is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }
static int is_id_start(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
         (c & 0x80); /* allow UTF-8 lead/continuation bytes in names */
}
static int is_id_cont(int c) { return is_id_start(c) || is_digit(c); }
static int is_space(int c)
{
  return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n' ||
         c == (char)0xA0; /* NBSP low byte; good enough without full Unicode */
}

/* ------------------------------------------------------- cooked buffer ---- */

typedef struct {
  char *b;
  int len, cap;
  int failed;
} cbuf;

static void cb_init(cbuf *c)
{
  c->b = NULL;
  c->len = 0;
  c->cap = 0;
  c->failed = 0;
}
static void cb_free(cbuf *c)
{
  if (c->b)
    free(c->b);
}
static void cb_push(cbuf *c, int ch)
{
  if (c->failed)
    return;
  if (c->len + 1 > c->cap) {
    int cap = c->cap ? c->cap * 2 : 32;
    char *nb = (char *)realloc(c->b, (unsigned)cap);
    if (!nb) {
      c->failed = 1;
      return;
    }
    c->b = nb;
    c->cap = cap;
  }
  c->b[c->len++] = (char)ch;
}
static void cb_push_cp(cbuf *c, unsigned cp)
{
  char t[4];
  int k = utf8_encode(cp, t), i;
  for (i = 0; i < k; i++)
    cb_push(c, t[i]);
}

/* ------------------------------------------------------- keyword table ---- */

static int eqn(const char *s, const char *kw, int n)
{
  int i;
  for (i = 0; i < n; i++)
    if (s[i] != kw[i])
      return 0;
  return 1;
}

static int keyword_kind(const char *s, int n)
{
  /* reserved words only; contextual keywords stay T_IDENT */
  switch (n) {
  case 2:
    if (eqn(s, "do", 2))
      return T_DO;
    if (eqn(s, "if", 2))
      return T_IF;
    if (eqn(s, "in", 2))
      return T_IN;
    break;
  case 3:
    if (eqn(s, "for", 3))
      return T_FOR;
    if (eqn(s, "new", 3))
      return T_NEW;
    if (eqn(s, "try", 3))
      return T_TRY;
    if (eqn(s, "var", 3))
      return T_VAR;
    break;
  case 4:
    if (eqn(s, "case", 4))
      return T_CASE;
    if (eqn(s, "else", 4))
      return T_ELSE;
    if (eqn(s, "null", 4))
      return T_NULL;
    if (eqn(s, "this", 4))
      return T_THIS;
    if (eqn(s, "true", 4))
      return T_TRUE;
    if (eqn(s, "void", 4))
      return T_VOID;
    if (eqn(s, "with", 4))
      return T_WITH;
    break;
  case 5:
    if (eqn(s, "break", 5))
      return T_BREAK;
    if (eqn(s, "catch", 5))
      return T_CATCH;
    if (eqn(s, "class", 5))
      return T_CLASS;
    if (eqn(s, "const", 5))
      return T_CONST;
    if (eqn(s, "false", 5))
      return T_FALSE;
    if (eqn(s, "super", 5))
      return T_SUPER;
    if (eqn(s, "throw", 5))
      return T_THROW;
    if (eqn(s, "while", 5))
      return T_WHILE;
    break;
  case 6:
    if (eqn(s, "delete", 6))
      return T_DELETE;
    if (eqn(s, "export", 6))
      return T_EXPORT;
    if (eqn(s, "import", 6))
      return T_IMPORT;
    if (eqn(s, "return", 6))
      return T_RETURN;
    if (eqn(s, "switch", 6))
      return T_SWITCH;
    if (eqn(s, "typeof", 6))
      return T_TYPEOF;
    break;
  case 7:
    if (eqn(s, "default", 7))
      return T_DEFAULT;
    if (eqn(s, "extends", 7))
      return T_EXTENDS;
    if (eqn(s, "finally", 7))
      return T_FINALLY;
    break;
  case 8:
    if (eqn(s, "continue", 8))
      return T_CONTINUE;
    if (eqn(s, "debugger", 8))
      return T_DEBUGGER;
    if (eqn(s, "function", 8))
      return T_FUNCTION;
    break;
  case 10:
    if (eqn(s, "instanceof", 10))
      return T_INSTANCEOF;
    break;
  }
  return T_IDENT;
}

/* ----------------------------------------------------------- init ----------
 */

void js_lexer_init(jlexer *lx, const char *src, js_arena *arena)
{
  lx->src = src;
  lx->p = src;
  lx->end = src + strlen(src);
  lx->line = 1;
  lx->line_start = src;
  lx->arena = arena;
  lx->prev_kind = 0;
  lx->saw_newline = 0;
  lx->brace_sp = 0;
  lx->paren_sp = 0;
  lx->prev_rparen_ctrl = 0;
  lx->prev_rbrace_block = 0;
  lx->err = NULL;
}

static void fail(jlexer *lx, jtoken *out, const char *msg)
{
  if (!lx->err)
    lx->err = msg;
  out->kind = T_ERROR;
}

/* ------------------------------------------------------- trivia ----------- */

/* Skip whitespace and comments; record whether a line terminator was crossed
 * (the parser needs this for automatic semicolon insertion). */
static void skip_trivia(jlexer *lx)
{
  for (;;) {
    char c = *lx->p;
    if (c == 0)
      return;
    if (c == '\n') {
      lx->line++;
      lx->saw_newline = 1;
      lx->p++;
      lx->line_start = lx->p;
      continue;
    }
    if (is_space(c)) {
      lx->p++;
      continue;
    }
    if (c == '/' && lx->p[1] == '/') {
      lx->p += 2;
      while (*lx->p && *lx->p != '\n')
        lx->p++;
      continue;
    }
    if (c == '/' && lx->p[1] == '*') {
      lx->p += 2;
      while (*lx->p && !(*lx->p == '*' && lx->p[1] == '/')) {
        if (*lx->p == '\n') {
          lx->line++;
          lx->saw_newline = 1;
          lx->line_start = lx->p + 1;
        }
        lx->p++;
      }
      if (*lx->p)
        lx->p += 2; /* consume the closing star-slash */
      continue;
    }
    /* Annex B HTML-like comments (legacy inline-script wrappers): `<!--`
     * opens a single-line comment anywhere; `-->` is one only at line start
     * (saw_newline => only whitespace/comments precede it on this line, so a
     * real `a-->b` post-decrement stays tokenized as `--` `>`). */
    if (c == '<' && lx->p[1] == '!' && lx->p[2] == '-' && lx->p[3] == '-') {
      lx->p += 4;
      while (*lx->p && *lx->p != '\n')
        lx->p++;
      continue;
    }
    if (c == '-' && lx->p[1] == '-' && lx->p[2] == '>' && lx->saw_newline) {
      lx->p += 3;
      while (*lx->p && *lx->p != '\n')
        lx->p++;
      continue;
    }
    return;
  }
}

/* ------------------------------------------------------- escapes ---------- */

/* Decode one backslash escape at lx->p (which points at '\\') into `cook`.
 * Returns 0 on a malformed escape. */
static int decode_escape(jlexer *lx, cbuf *cook)
{
  char c;
  lx->p++; /* past '\\' */
  c = *lx->p;
  if (c == 0)
    return 0;
  switch (c) {
  case 'n':
    cb_push(cook, '\n');
    lx->p++;
    return 1;
  case 't':
    cb_push(cook, '\t');
    lx->p++;
    return 1;
  case 'r':
    cb_push(cook, '\r');
    lx->p++;
    return 1;
  case 'b':
    cb_push(cook, '\b');
    lx->p++;
    return 1;
  case 'f':
    cb_push(cook, '\f');
    lx->p++;
    return 1;
  case 'v':
    cb_push(cook, '\v');
    lx->p++;
    return 1;
  case '\r':
    if (lx->p[1] == '\n')
      lx->p++; /* fall through */
  case '\n':
    lx->line++;
    lx->p++;
    lx->line_start = lx->p;
    return 1; /* line continuation: emit nothing */
  case 'x': {
    if (!is_hex(lx->p[1]) || !is_hex(lx->p[2]))
      return 0;
    cb_push_cp(cook, (unsigned)(hexval(lx->p[1]) * 16 + hexval(lx->p[2])));
    lx->p += 3;
    return 1;
  }
  case 'u': {
    unsigned cp = 0;
    lx->p++;
    if (*lx->p == '{') {
      lx->p++;
      if (!is_hex(*lx->p))
        return 0;
      while (is_hex(*lx->p)) {
        cp = cp * 16 + (unsigned)hexval(*lx->p);
        lx->p++;
      }
      if (*lx->p != '}')
        return 0;
      lx->p++;
    } else {
      int i;
      for (i = 0; i < 4; i++) {
        if (!is_hex(*lx->p))
          return 0;
        cp = cp * 16 + (unsigned)hexval(*lx->p);
        lx->p++;
      }
    }
    cb_push_cp(cook, cp);
    return 1;
  }
  default:
    if (c >= '0' && c <= '7') { /* legacy octal escape, up to 3 digits */
      unsigned v = 0;
      int i = 0;
      while (i < 3 && *lx->p >= '0' && *lx->p <= '7') {
        v = v * 8 + (unsigned)(*lx->p - '0');
        lx->p++;
        i++;
      }
      cb_push_cp(cook, v);
      return 1;
    }
    cb_push(cook, c); /* unknown escape: the char literally */
    lx->p++;
    return 1;
  }
}

/* ------------------------------------------------------- strings ---------- */

static void scan_string(jlexer *lx, jtoken *out, char quote)
{
  cbuf cook;
  cb_init(&cook);
  lx->p++; /* opening quote */
  for (;;) {
    char c = *lx->p;
    if (c == 0 || c == '\n') {
      cb_free(&cook);
      fail(lx, out, "unterminated string literal");
      return;
    }
    if (c == quote) {
      lx->p++;
      break;
    }
    if (c == '\\') {
      if (!decode_escape(lx, &cook)) {
        cb_free(&cook);
        fail(lx, out, "bad escape sequence");
        return;
      }
      continue;
    }
    cb_push(&cook, c);
    lx->p++;
  }
  out->kind = T_STR;
  if (cook.failed) {
    cb_free(&cook);
    fail(lx, out, "out of memory");
    return;
  }
  out->value = js_arena_strndup(lx->arena, cook.b ? cook.b : "", cook.len);
  if (!out->value) {
    cb_free(&cook);
    fail(lx, out, "out of memory");
    return;
  }
  out->value_len = cook.len;
  cb_free(&cook);
}

/* ------------------------------------------------------- templates -------- */

/* Scan a template segment from lx->p (just after a '`' or after a '}' that
 * closed a substitution). On '${' emits HEAD/MIDDLE and pushes a substitution
 * marker; on '`' emits TEMPLATE/TAIL. `after_sub` distinguishes the two cases.
 */
static void scan_template(jlexer *lx, jtoken *out, int after_sub)
{
  cbuf cook;
  cb_init(&cook);
  for (;;) {
    char c = *lx->p;
    if (c == 0) {
      cb_free(&cook);
      fail(lx, out, "unterminated template literal");
      return;
    }
    if (c == '`') {
      lx->p++;
      out->kind = after_sub ? T_TEMPLATE_TAIL : T_TEMPLATE;
      break;
    }
    if (c == '$' && lx->p[1] == '{') {
      lx->p += 2;
      out->kind = after_sub ? T_TEMPLATE_MIDDLE : T_TEMPLATE_HEAD;
      if (lx->brace_sp < (int)sizeof(lx->brace))
        lx->brace[lx->brace_sp++] = 1;
      break;
    }
    if (c == '\\') {
      if (!decode_escape(lx, &cook)) {
        cb_free(&cook);
        fail(lx, out, "bad escape sequence");
        return;
      }
      continue;
    }
    if (c == '\r') {
      lx->line++;
      if (lx->p[1] == '\n')
        lx->p++;
      cb_push(&cook, '\n');
      lx->p++;
      lx->line_start = lx->p;
      continue;
    }
    if (c == '\n') {
      lx->line++;
      cb_push(&cook, '\n');
      lx->p++;
      lx->line_start = lx->p;
      continue;
    }
    cb_push(&cook, c);
    lx->p++;
  }
  if (cook.failed) {
    cb_free(&cook);
    fail(lx, out, "out of memory");
    return;
  }
  out->value = js_arena_strndup(lx->arena, cook.b ? cook.b : "", cook.len);
  if (!out->value) {
    cb_free(&cook);
    fail(lx, out, "out of memory");
    return;
  }
  out->value_len = cook.len;
  cb_free(&cook);
}

/* ------------------------------------------------------- numbers ---------- */

static void scan_number(jlexer *lx, jtoken *out)
{
  const char *s = lx->p;
  double val = 0;
  int is_int = 1;

  if (*lx->p == '0' && (lx->p[1] == 'x' || lx->p[1] == 'X')) {
    lx->p += 2;
    if (!is_hex(*lx->p)) {
      fail(lx, out, "missing hex digits");
      return;
    }
    while (is_hex(*lx->p) || *lx->p == '_') {
      if (*lx->p != '_')
        val = val * 16 + hexval(*lx->p);
      lx->p++;
    }
  } else if (*lx->p == '0' && (lx->p[1] == 'o' || lx->p[1] == 'O')) {
    lx->p += 2;
    if (!(*lx->p >= '0' && *lx->p <= '7')) {
      fail(lx, out, "missing octal digits");
      return;
    }
    while ((*lx->p >= '0' && *lx->p <= '7') || *lx->p == '_') {
      if (*lx->p != '_')
        val = val * 8 + (*lx->p - '0');
      lx->p++;
    }
  } else if (*lx->p == '0' && (lx->p[1] == 'b' || lx->p[1] == 'B')) {
    lx->p += 2;
    if (!(*lx->p == '0' || *lx->p == '1')) {
      fail(lx, out, "missing binary digits");
      return;
    }
    while (*lx->p == '0' || *lx->p == '1' || *lx->p == '_') {
      if (*lx->p != '_')
        val = val * 2 + (*lx->p - '0');
      lx->p++;
    }
  } else if (*lx->p == '0' && lx->p[1] >= '0' && lx->p[1] <= '9') {
    const char *q = lx->p + 1;
    int oct = 1;
    while (*q >= '0' && *q <= '9') {
      if (*q > '7')
        oct = 0;
      q++;
    }
    if (*q == '.' || *q == 'e' || *q == 'E')
      oct = 0;
    if (oct) {
      while (*lx->p >= '0' && *lx->p <= '7') {
        val = val * 8 + (*lx->p - '0');
        lx->p++;
      }
    } else {
      goto decimal;
    }
  } else {
  decimal: {
    char tmp[160];
    int ti = 0;
    while (is_digit(*lx->p) || *lx->p == '_') {
      if (*lx->p != '_' && ti < 158)
        tmp[ti++] = *lx->p;
      lx->p++;
    }
    if (*lx->p == '.') {
      is_int = 0;
      if (ti < 158)
        tmp[ti++] = '.';
      lx->p++;
      while (is_digit(*lx->p) || *lx->p == '_') {
        if (*lx->p != '_' && ti < 158)
          tmp[ti++] = *lx->p;
        lx->p++;
      }
    }
    if (*lx->p == 'e' || *lx->p == 'E') {
      is_int = 0;
      if (ti < 158)
        tmp[ti++] = 'e';
      lx->p++;
      if (*lx->p == '+' || *lx->p == '-') {
        if (ti < 158)
          tmp[ti++] = *lx->p;
        lx->p++;
      }
      while (is_digit(*lx->p) || *lx->p == '_') {
        if (*lx->p != '_' && ti < 158)
          tmp[ti++] = *lx->p;
        lx->p++;
      }
    }
    tmp[ti] = 0;
    val = strtod(tmp, NULL);
  }
  }

  out->num = val;
  if (is_int && *lx->p == 'n') {
    lx->p++;
    out->kind = T_BIGINT;
  } else
    out->kind = T_NUM;
  out->start = s;
  out->len = (int)(lx->p - s);
}

/* ------------------------------------------------------- regex ------------ */

/* True if a `/` in this position begins a regex literal rather than division,
 * decided from the previous significant token. */
static int regex_allowed(int prev)
{
  switch (prev) {
  case T_IDENT:
  case T_NUM:
  case T_BIGINT:
  case T_STR:
  case T_REGEX:
  case T_TEMPLATE:
  case T_TEMPLATE_TAIL:
  case T_RPAREN:
  case T_RBRACK:
  case T_RBRACE:
  case T_PLUSPLUS:
  case T_MINUSMINUS:
  case T_THIS:
  case T_SUPER:
  case T_NULL:
  case T_TRUE:
  case T_FALSE:
    return 0;
  default:
    return 1;
  }
}

/* True if a `{` in this position opens an object literal rather than a block.
 * Decided from the previous significant token: a `{` is an object literal only
 * in expression-operand position (after an operator, `(`/`[`/`,`/`:`/`?`, or an
 * expression-introducing keyword like return/typeof/new). Everywhere else —
 * statement start, after `;`/`}`/`)`/`=>`/`else`, a function or control body —
 * it opens a block. This drives the regex-vs-divide choice after the matching
 * `}`: a `/` after a block-`}` is a regex; after an object-literal `}`, divide.
 */
static int lbrace_is_object(int prev)
{
  switch (prev) {
  case T_LPAREN:
  case T_LBRACK:
  case T_COMMA:
  case T_COLON:
  case T_QUESTION:
  case T_EQ:
  case T_PLUS_EQ:
  case T_MINUS_EQ:
  case T_STAR_EQ:
  case T_SLASH_EQ:
  case T_PERCENT_EQ:
  case T_STARSTAR_EQ:
  case T_LTLT_EQ:
  case T_GTGT_EQ:
  case T_GTGTGT_EQ:
  case T_AMP_EQ:
  case T_BAR_EQ:
  case T_CARET_EQ:
  case T_AMPAMP_EQ:
  case T_BARBAR_EQ:
  case T_QQ_EQ:
  case T_PLUS:
  case T_MINUS:
  case T_STAR:
  case T_PERCENT:
  case T_STARSTAR:
  case T_EQEQ:
  case T_NE:
  case T_EQEQEQ:
  case T_NEEQ:
  case T_LT:
  case T_GT:
  case T_LE:
  case T_GE:
  case T_LTLT:
  case T_GTGT:
  case T_GTGTGT:
  case T_AMP:
  case T_BAR:
  case T_CARET:
  case T_AMPAMP:
  case T_BARBAR:
  case T_QQ:
  case T_BANG:
  case T_TILDE:
  case T_ELLIPSIS:
  case T_IN:
  case T_INSTANCEOF:
  case T_RETURN:
  case T_TYPEOF:
  case T_VOID:
  case T_DELETE:
  case T_NEW:
  case T_THROW:
  case T_CASE:
    return 1;
  default:
    return 0;
  }
}

static void scan_regex(jlexer *lx, jtoken *out)
{
  cbuf pat, flags;
  int in_class = 0;
  cb_init(&pat);
  cb_init(&flags);
  lx->p++; /* opening '/' */
  for (;;) {
    char c = *lx->p;
    if (c == 0 || c == '\n') {
      cb_free(&pat);
      cb_free(&flags);
      fail(lx, out, "unterminated regex literal");
      return;
    }
    if (c == '\\') {
      cb_push(&pat, c);
      if (lx->p[1]) {
        cb_push(&pat, lx->p[1]);
        lx->p += 2;
      } else
        lx->p++;
      continue;
    }
    if (c == '[')
      in_class = 1;
    else if (c == ']')
      in_class = 0;
    else if (c == '/' && !in_class) {
      lx->p++;
      break;
    }
    cb_push(&pat, c);
    lx->p++;
  }
  while (is_id_cont(*lx->p)) {
    cb_push(&flags, *lx->p);
    lx->p++;
  }
  if (pat.failed || flags.failed) {
    cb_free(&pat);
    cb_free(&flags);
    fail(lx, out, "out of memory");
    return;
  }
  out->kind = T_REGEX;
  out->value = js_arena_strndup(lx->arena, pat.b ? pat.b : "", pat.len);
  if (!out->value) {
    cb_free(&pat);
    cb_free(&flags);
    fail(lx, out, "out of memory");
    return;
  }
  out->value_len = pat.len;
  out->flags = js_arena_strndup(lx->arena, flags.b ? flags.b : "", flags.len);
  if (!out->flags) {
    cb_free(&pat);
    cb_free(&flags);
    fail(lx, out, "out of memory");
    return;
  }
  cb_free(&pat);
  cb_free(&flags);
}

/* ------------------------------------------------------- identifiers ------ */

/* Decode a '\uXXXX' or '\u{...}' escape at lx->p (pointing at the '\\') — the
 * only escape legal inside an IdentifierName — appending its UTF-8 form to
 * cook. Returns 0 on a non-'\u' or malformed escape. */
static int decode_ident_escape(jlexer *lx, cbuf *cook)
{
  unsigned cp = 0;
  if (lx->p[0] != '\\' || lx->p[1] != 'u')
    return 0;
  lx->p += 2;
  if (*lx->p == '{') {
    lx->p++;
    if (!is_hex(*lx->p))
      return 0;
    while (is_hex(*lx->p)) {
      cp = cp * 16 + (unsigned)hexval(*lx->p);
      lx->p++;
    }
    if (*lx->p != '}')
      return 0;
    lx->p++;
  } else {
    int i;
    for (i = 0; i < 4; i++) {
      if (!is_hex(*lx->p))
        return 0;
      cp = cp * 16 + (unsigned)hexval(*lx->p);
      lx->p++;
    }
  }
  cb_push_cp(cook, cp);
  return 1;
}

/* Scan an identifier containing one or more '\uXXXX' escapes (the slow path;
 * plain names take the raw-slice fast path in js_lex_next). Re-scans from `s`,
 * decoding each escape into its UTF-8 bytes so the cooked name is the
 * identifier's StringValue. An escaped identifier is never a keyword (e.g. `if`
 * is the name "if", not the `if` keyword). */
static void scan_ident_escaped(jlexer *lx, jtoken *out, const char *s)
{
  cbuf cook;
  int first = 1;
  cb_init(&cook);
  lx->p = s;
  for (;;) {
    char c = *lx->p;
    if (c == '\\' && lx->p[1] == 'u') {
      if (!decode_ident_escape(lx, &cook)) {
        cb_free(&cook);
        fail(lx, out, "invalid Unicode escape in identifier");
        return;
      }
    } else if (first ? is_id_start((unsigned char)c) : is_id_cont((unsigned char)c)) {
      cb_push(&cook, c);
      lx->p++;
    } else
      break;
    first = 0;
  }
  out->len = (int)(lx->p - s);
  if (cook.failed) {
    cb_free(&cook);
    fail(lx, out, "out of memory");
    return;
  }
  out->value = js_arena_strndup(lx->arena, cook.b ? cook.b : "", cook.len);
  if (!out->value) {
    cb_free(&cook);
    fail(lx, out, "out of memory");
    return;
  }
  out->value_len = cook.len;
  out->kind = T_IDENT;
  cb_free(&cook);
}

/* ------------------------------------------------------- main scan -------- */

void js_lex_next(jlexer *lx, jtoken *out)
{
  char c;
  int rparen_ctrl, rbrace_block;
  out->kind = T_EOF;
  out->newline_before = 0;
  out->num = 0;
  out->value = NULL;
  out->value_len = 0;
  out->flags = NULL;
  out->len = 0;
  out->col = 1;

  if (lx->err) {
    out->kind = T_ERROR;
    return;
  }

  skip_trivia(lx);
  out->newline_before = lx->saw_newline;
  lx->saw_newline = 0;
  out->line = lx->line;
  out->col = (int)(lx->p - lx->line_start) + 1;
  out->start = lx->p;

  c = *lx->p;
  /* Carry the "previous token was a control-flow header ')'" flag across the
   * trivia between that ')' and this token, then clear it; re-set below only
   * when this token is itself such a ')'. */
  rparen_ctrl = lx->prev_rparen_ctrl;
  lx->prev_rparen_ctrl = 0;
  rbrace_block = lx->prev_rbrace_block;
  lx->prev_rbrace_block = 0;
  if (c == 0) {
    out->kind = T_EOF;
    lx->prev_kind = T_EOF;
    return;
  }

  if (is_id_start(c)) {
    const char *s = lx->p;
    while (is_id_cont(*lx->p))
      lx->p++;
    if (*lx->p == '\\' && lx->p[1] == 'u') { /* embedded \uXXXX: cook the name */
      scan_ident_escaped(lx, out, s);
      if (out->kind != T_ERROR)
        lx->prev_kind = out->kind;
      return;
    }
    out->len = (int)(lx->p - s);
    out->value = js_arena_strndup(lx->arena, s, out->len);
    if (!out->value) {
      fail(lx, out, "out of memory");
      return;
    }
    out->value_len = out->len;
    out->kind = keyword_kind(s, out->len);
    lx->prev_kind = out->kind;
    return;
  }

  /* An identifier that begins with a Unicode escape (e.g. `Ā` as an object
   * key — real-world minifiers/transliteration maps emit these). */
  if (c == '\\' && lx->p[1] == 'u') {
    scan_ident_escaped(lx, out, lx->p);
    if (out->kind != T_ERROR)
      lx->prev_kind = out->kind;
    return;
  }

  /* A private class name (#field / #method): lexed as an ordinary identifier
   * token whose text keeps the leading '#', so it flows through the normal
   * property-key / member-access machinery as the key "#name". */
  if (c == '#' && is_id_start((unsigned char)lx->p[1])) {
    const char *s = lx->p;
    lx->p++; /* '#' */
    while (is_id_cont(*lx->p))
      lx->p++;
    out->len = (int)(lx->p - s);
    out->value = js_arena_strndup(lx->arena, s, out->len);
    if (!out->value) {
      fail(lx, out, "out of memory");
      return;
    }
    out->value_len = out->len;
    out->kind = T_IDENT;
    lx->prev_kind = out->kind;
    return;
  }

  if (is_digit(c) || (c == '.' && is_digit(lx->p[1]))) {
    scan_number(lx, out);
    if (out->kind != T_ERROR)
      lx->prev_kind = out->kind;
    return;
  }

  if (c == '"' || c == '\'') {
    scan_string(lx, out, c);
    out->len = (int)(lx->p - out->start);
    if (out->kind != T_ERROR)
      lx->prev_kind = out->kind;
    return;
  }

  if (c == '`') {
    lx->p++;
    scan_template(lx, out, 0);
    out->len = (int)(lx->p - out->start);
    if (out->kind != T_ERROR)
      lx->prev_kind = out->kind;
    return;
  }

  if (c == '}') {
    if (lx->brace_sp > 0 && lx->brace[lx->brace_sp - 1] == 1) {
      lx->brace_sp--;
      lx->p++; /* past the '}' that closes ${...} */
      scan_template(lx, out, 1);
      out->len = (int)(lx->p - out->start);
      if (out->kind != T_ERROR)
        lx->prev_kind = out->kind;
      return;
    }
    if (lx->brace_sp > 0 && lx->brace[lx->brace_sp - 1] == 2)
      lx->prev_rbrace_block = 1;
    if (lx->brace_sp > 0)
      lx->brace_sp--;
    lx->p++;
    out->kind = T_RBRACE;
    out->len = 1;
    lx->prev_kind = T_RBRACE;
    return;
  }

  if (c == '/' && (regex_allowed(lx->prev_kind) || rparen_ctrl || rbrace_block)) {
    scan_regex(lx, out);
    out->len = (int)(lx->p - out->start);
    if (out->kind != T_ERROR)
      lx->prev_kind = out->kind;
    return;
  }

#define ADV(n, k)                                                                                                      \
  do {                                                                                                                 \
    lx->p += (n);                                                                                                      \
    out->kind = (k);                                                                                                   \
    out->len = (n);                                                                                                    \
    lx->prev_kind = (k);                                                                                               \
    return;                                                                                                            \
  } while (0)
  {
    char d = lx->p[1];
    switch (c) {
    case '{':
      if (lx->brace_sp < (int)sizeof(lx->brace))
        lx->brace[lx->brace_sp++] = lbrace_is_object(lx->prev_kind) ? 0 : 2;
      ADV(1, T_LBRACE);
    case '(': {
      /* Remember whether this '(' is an if/for/while/with header: a ')'
       * closing it is followed by a statement, so a '/' there is a regex. */
      int ctrl =
          (lx->prev_kind == T_IF || lx->prev_kind == T_FOR || lx->prev_kind == T_WHILE || lx->prev_kind == T_WITH);
      if (lx->paren_sp < (int)sizeof(lx->paren))
        lx->paren[lx->paren_sp++] = (unsigned char)ctrl;
      ADV(1, T_LPAREN);
    }
    case ')': {
      int ctrl = 0;
      if (lx->paren_sp > 0)
        ctrl = lx->paren[--lx->paren_sp];
      lx->p += 1;
      out->kind = T_RPAREN;
      out->len = 1;
      lx->prev_kind = T_RPAREN;
      lx->prev_rparen_ctrl = ctrl;
      return;
    }
    case '[':
      ADV(1, T_LBRACK);
    case ']':
      ADV(1, T_RBRACK);
    case ';':
      ADV(1, T_SEMI);
    case ',':
      ADV(1, T_COMMA);
    case ':':
      ADV(1, T_COLON);
    case '~':
      ADV(1, T_TILDE);
    case '.':
      if (d == '.' && lx->p[2] == '.')
        ADV(3, T_ELLIPSIS);
      ADV(1, T_DOT);
    case '?':
      if (d == '?') {
        if (lx->p[2] == '=')
          ADV(3, T_QQ_EQ);
        ADV(2, T_QQ);
      }
      if (d == '.' && !is_digit(lx->p[2]))
        ADV(2, T_QDOT);
      ADV(1, T_QUESTION);
    case '=':
      if (d == '=') {
        if (lx->p[2] == '=')
          ADV(3, T_EQEQEQ);
        ADV(2, T_EQEQ);
      }
      if (d == '>')
        ADV(2, T_ARROW);
      ADV(1, T_EQ);
    case '!':
      if (d == '=') {
        if (lx->p[2] == '=')
          ADV(3, T_NEEQ);
        ADV(2, T_NE);
      }
      ADV(1, T_BANG);
    case '<':
      if (d == '<') {
        if (lx->p[2] == '=')
          ADV(3, T_LTLT_EQ);
        ADV(2, T_LTLT);
      }
      if (d == '=')
        ADV(2, T_LE);
      ADV(1, T_LT);
    case '>':
      if (d == '>') {
        if (lx->p[2] == '>') {
          if (lx->p[3] == '=')
            ADV(4, T_GTGTGT_EQ);
          ADV(3, T_GTGTGT);
        }
        if (lx->p[2] == '=')
          ADV(3, T_GTGT_EQ);
        ADV(2, T_GTGT);
      }
      if (d == '=')
        ADV(2, T_GE);
      ADV(1, T_GT);
    case '+':
      if (d == '+')
        ADV(2, T_PLUSPLUS);
      if (d == '=')
        ADV(2, T_PLUS_EQ);
      ADV(1, T_PLUS);
    case '-':
      if (d == '-')
        ADV(2, T_MINUSMINUS);
      if (d == '=')
        ADV(2, T_MINUS_EQ);
      ADV(1, T_MINUS);
    case '*':
      if (d == '*') {
        if (lx->p[2] == '=')
          ADV(3, T_STARSTAR_EQ);
        ADV(2, T_STARSTAR);
      }
      if (d == '=')
        ADV(2, T_STAR_EQ);
      ADV(1, T_STAR);
    case '/':
      if (d == '=')
        ADV(2, T_SLASH_EQ);
      ADV(1, T_SLASH);
    case '%':
      if (d == '=')
        ADV(2, T_PERCENT_EQ);
      ADV(1, T_PERCENT);
    case '&':
      if (d == '&') {
        if (lx->p[2] == '=')
          ADV(3, T_AMPAMP_EQ);
        ADV(2, T_AMPAMP);
      }
      if (d == '=')
        ADV(2, T_AMP_EQ);
      ADV(1, T_AMP);
    case '|':
      if (d == '|') {
        if (lx->p[2] == '=')
          ADV(3, T_BARBAR_EQ);
        ADV(2, T_BARBAR);
      }
      if (d == '=')
        ADV(2, T_BAR_EQ);
      ADV(1, T_BAR);
    case '^':
      if (d == '=')
        ADV(2, T_CARET_EQ);
      ADV(1, T_CARET);
    }
  }
#undef ADV

  fail(lx, out, "unexpected character");
}

/* ------------------------------------------------------- spelling --------- */

const char *js_tok_spell(int kind)
{
  switch (kind) {
  case T_EOF:
    return "end of input";
  case T_ERROR:
    return "error";
  case T_IDENT:
    return "identifier";
  case T_NUM:
    return "number";
  case T_BIGINT:
    return "bigint";
  case T_STR:
    return "string";
  case T_REGEX:
    return "regex";
  case T_TEMPLATE:
  case T_TEMPLATE_HEAD:
  case T_TEMPLATE_MIDDLE:
  case T_TEMPLATE_TAIL:
    return "template";
  case T_BREAK:
    return "break";
  case T_CASE:
    return "case";
  case T_CATCH:
    return "catch";
  case T_CLASS:
    return "class";
  case T_CONST:
    return "const";
  case T_CONTINUE:
    return "continue";
  case T_DEBUGGER:
    return "debugger";
  case T_DEFAULT:
    return "default";
  case T_DELETE:
    return "delete";
  case T_DO:
    return "do";
  case T_ELSE:
    return "else";
  case T_EXPORT:
    return "export";
  case T_EXTENDS:
    return "extends";
  case T_FINALLY:
    return "finally";
  case T_FOR:
    return "for";
  case T_FUNCTION:
    return "function";
  case T_IF:
    return "if";
  case T_IMPORT:
    return "import";
  case T_IN:
    return "in";
  case T_INSTANCEOF:
    return "instanceof";
  case T_NEW:
    return "new";
  case T_RETURN:
    return "return";
  case T_SUPER:
    return "super";
  case T_SWITCH:
    return "switch";
  case T_THIS:
    return "this";
  case T_THROW:
    return "throw";
  case T_TRY:
    return "try";
  case T_TYPEOF:
    return "typeof";
  case T_VAR:
    return "var";
  case T_VOID:
    return "void";
  case T_WHILE:
    return "while";
  case T_WITH:
    return "with";
  case T_NULL:
    return "null";
  case T_TRUE:
    return "true";
  case T_FALSE:
    return "false";
  case T_LBRACE:
    return "{";
  case T_RBRACE:
    return "}";
  case T_LPAREN:
    return "(";
  case T_RPAREN:
    return ")";
  case T_LBRACK:
    return "[";
  case T_RBRACK:
    return "]";
  case T_DOT:
    return ".";
  case T_ELLIPSIS:
    return "...";
  case T_SEMI:
    return ";";
  case T_COMMA:
    return ",";
  case T_LT:
    return "<";
  case T_GT:
    return ">";
  case T_LE:
    return "<=";
  case T_GE:
    return ">=";
  case T_EQEQ:
    return "==";
  case T_NE:
    return "!=";
  case T_EQEQEQ:
    return "===";
  case T_NEEQ:
    return "!==";
  case T_PLUS:
    return "+";
  case T_MINUS:
    return "-";
  case T_STAR:
    return "*";
  case T_PERCENT:
    return "%";
  case T_STARSTAR:
    return "**";
  case T_SLASH:
    return "/";
  case T_PLUSPLUS:
    return "++";
  case T_MINUSMINUS:
    return "--";
  case T_LTLT:
    return "<<";
  case T_GTGT:
    return ">>";
  case T_GTGTGT:
    return ">>>";
  case T_AMP:
    return "&";
  case T_BAR:
    return "|";
  case T_CARET:
    return "^";
  case T_BANG:
    return "!";
  case T_TILDE:
    return "~";
  case T_AMPAMP:
    return "&&";
  case T_BARBAR:
    return "||";
  case T_QQ:
    return "??";
  case T_QUESTION:
    return "?";
  case T_QDOT:
    return "?.";
  case T_COLON:
    return ":";
  case T_ARROW:
    return "=>";
  case T_EQ:
    return "=";
  case T_PLUS_EQ:
    return "+=";
  case T_MINUS_EQ:
    return "-=";
  case T_STAR_EQ:
    return "*=";
  case T_SLASH_EQ:
    return "/=";
  case T_PERCENT_EQ:
    return "%=";
  case T_STARSTAR_EQ:
    return "**=";
  case T_LTLT_EQ:
    return "<<=";
  case T_GTGT_EQ:
    return ">>=";
  case T_GTGTGT_EQ:
    return ">>>=";
  case T_AMP_EQ:
    return "&=";
  case T_BAR_EQ:
    return "|=";
  case T_CARET_EQ:
    return "^=";
  case T_AMPAMP_EQ:
    return "&&=";
  case T_BARBAR_EQ:
    return "||=";
  case T_QQ_EQ:
    return "?\?=";
  default:
    return "?";
  }
}

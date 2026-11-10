/* json.c — JSON value tree: a strict RFC 8259 parser, a programmatic builder,
 * and a serializer. See json.h for the API and ownership contract. */
#include "json.h"
#include "fmt.h" /* dtoa / strtod — shortest round-trip number conversion */
#include "owos.h"

#define JSON_MAX_DEPTH 200 /* bounds parser recursion: deep input can't blow the stack */

/* ---------------------------------------------------------- growable buffer */

typedef struct {
  char *b;
  int n, cap;
} sbuf;

static int sb_grow(sbuf *s, int need)
{
  int nc;
  char *nb;
  if (need < 0 || s->n > 2147483647 - need)
    return -1; /* n+need would overflow int */
  if (s->n + need <= s->cap)
    return 0;
  nc = s->cap ? s->cap : 64;
  while (nc < s->n + need) {
    if (nc > 2147483647 / 2) {
      nc = s->n + need;
      break;
    } /* avoid *2 wrapping negative */
    nc *= 2;
  }
  nb = realloc(s->b, (unsigned)nc);
  if (!nb)
    return -1;
  s->b = nb;
  s->cap = nc;
  return 0;
}
static int sb_put(sbuf *s, const char *p, int len)
{
  if (sb_grow(s, len))
    return -1;
  memcpy(s->b + s->n, p, (unsigned)len);
  s->n += len;
  return 0;
}
static int sb_putc(sbuf *s, char c)
{
  if (sb_grow(s, 1))
    return -1;
  s->b[s->n++] = c;
  return 0;
}
static int sb_puts(sbuf *s, const char *p) { return sb_put(s, p, (int)strlen(p)); }

/* ------------------------------------------------------------- constructors */

static json_value *val_new(json_type t)
{
  json_value *v = calloc(1, sizeof *v);
  if (v)
    v->type = t;
  return v;
}

json_value *json_new_null(void) { return val_new(JSON_NULL); }
json_value *json_new_array(void) { return val_new(JSON_ARRAY); }
json_value *json_new_object(void) { return val_new(JSON_OBJECT); }

json_value *json_new_bool(int b)
{
  json_value *v = val_new(JSON_BOOL);
  if (v)
    v->u.boolean = b ? 1 : 0;
  return v;
}
json_value *json_new_number(double n)
{
  json_value *v = val_new(JSON_NUMBER);
  if (v)
    v->u.number = n;
  return v;
}
json_value *json_new_stringn(const char *s, int len)
{
  json_value *v;
  char *d;
  if (len < 0)
    len = 0;
  v = val_new(JSON_STRING);
  if (!v)
    return 0;
  d = malloc((unsigned)len + 1);
  if (!d) {
    free(v);
    return 0;
  }
  if (len)
    memcpy(d, s, (unsigned)len);
  d[len] = 0;
  v->u.string.data = d;
  v->u.string.len = len;
  return v;
}
json_value *json_new_string(const char *s) { return json_new_stringn(s, s ? (int)strlen(s) : 0); }

/* ----------------------------------------------------------------- builder */

int json_array_push(json_value *arr, json_value *item)
{
  if (!arr || arr->type != JSON_ARRAY || !item)
    return -1;
  if (arr->u.array.count == arr->u.array.cap) {
    int nc = arr->u.array.cap ? arr->u.array.cap * 2 : 8;
    json_value **ni = realloc(arr->u.array.items, (unsigned)nc * sizeof *ni);
    if (!ni)
      return -1;
    arr->u.array.items = ni;
    arr->u.array.cap = nc;
  }
  arr->u.array.items[arr->u.array.count++] = item;
  return 0;
}

int json_object_set(json_value *obj, const char *key, json_value *val)
{
  int klen, i;
  char *kc;
  if (!obj || obj->type != JSON_OBJECT || !key || !val)
    return -1;

  for (i = 0; i < obj->u.object.count; i++)
    if (strcmp(obj->u.object.keys[i], key) == 0) { /* replace in place */
      json_free(obj->u.object.vals[i]);
      obj->u.object.vals[i] = val;
      return 0;
    }

  if (obj->u.object.count == obj->u.object.cap) {
    int nc = obj->u.object.cap ? obj->u.object.cap * 2 : 8;
    char **nk = realloc(obj->u.object.keys, (unsigned)nc * sizeof *nk);
    json_value **nv;
    if (!nk)
      return -1;
    obj->u.object.keys = nk;
    nv = realloc(obj->u.object.vals, (unsigned)nc * sizeof *nv);
    if (!nv)
      return -1;
    obj->u.object.vals = nv;
    obj->u.object.cap = nc;
  }
  klen = (int)strlen(key);
  kc = malloc((unsigned)klen + 1);
  if (!kc)
    return -1;
  memcpy(kc, key, (unsigned)klen + 1);
  obj->u.object.keys[obj->u.object.count] = kc;
  obj->u.object.vals[obj->u.object.count] = val;
  obj->u.object.count++;
  return 0;
}

int json_object_put(json_value *obj, const char *key, json_value *val)
{
  if (json_object_set(obj, key, val) == 0)
    return 0;
  json_free(val);
  return -1;
}

/* ---------------------------------------------------------------- accessors */

json_type json_typeof(const json_value *v) { return v ? v->type : JSON_NULL; }
int json_is_null(const json_value *v) { return !v || v->type == JSON_NULL; }
int json_as_bool(const json_value *v, int d) { return (v && v->type == JSON_BOOL) ? v->u.boolean : d; }
double json_as_number(const json_value *v, double d) { return (v && v->type == JSON_NUMBER) ? v->u.number : d; }
const char *json_as_string(const json_value *v) { return (v && v->type == JSON_STRING) ? v->u.string.data : 0; }
int json_string_len(const json_value *v) { return (v && v->type == JSON_STRING) ? v->u.string.len : 0; }

int json_array_len(const json_value *v) { return (v && v->type == JSON_ARRAY) ? v->u.array.count : 0; }
json_value *json_array_get(const json_value *v, int i)
{
  if (!v || v->type != JSON_ARRAY || i < 0 || i >= v->u.array.count)
    return 0;
  return v->u.array.items[i];
}

int json_object_len(const json_value *v) { return (v && v->type == JSON_OBJECT) ? v->u.object.count : 0; }
json_value *json_object_get(const json_value *v, const char *key)
{
  int i;
  if (!v || v->type != JSON_OBJECT || !key)
    return 0;
  for (i = 0; i < v->u.object.count; i++)
    if (strcmp(v->u.object.keys[i], key) == 0)
      return v->u.object.vals[i];
  return 0;
}
const char *json_object_key(const json_value *v, int i)
{
  if (!v || v->type != JSON_OBJECT || i < 0 || i >= v->u.object.count)
    return 0;
  return v->u.object.keys[i];
}
json_value *json_object_val(const json_value *v, int i)
{
  if (!v || v->type != JSON_OBJECT || i < 0 || i >= v->u.object.count)
    return 0;
  return v->u.object.vals[i];
}

/* ----------------------------------------------------------------- destroy */

void json_free(json_value *v)
{
  int i;
  if (!v)
    return;
  switch (v->type) {
  case JSON_STRING:
    free(v->u.string.data);
    break;
  case JSON_ARRAY:
    for (i = 0; i < v->u.array.count; i++)
      json_free(v->u.array.items[i]);
    free(v->u.array.items);
    break;
  case JSON_OBJECT:
    for (i = 0; i < v->u.object.count; i++) {
      free(v->u.object.keys[i]);
      json_free(v->u.object.vals[i]);
    }
    free(v->u.object.keys);
    free(v->u.object.vals);
    break;
  default:
    break;
  }
  free(v);
}

/* ------------------------------------------------------------------- clone */

json_value *json_clone(const json_value *v)
{
  int i;
  if (!v)
    return 0;
  switch (v->type) {
  case JSON_NULL:
    return json_new_null();
  case JSON_BOOL:
    return json_new_bool(v->u.boolean);
  case JSON_NUMBER:
    return json_new_number(v->u.number);
  case JSON_STRING:
    return json_new_stringn(v->u.string.data, v->u.string.len);
  case JSON_ARRAY: {
    json_value *a = json_new_array();
    if (!a)
      return 0;
    for (i = 0; i < v->u.array.count; i++) {
      json_value *c = json_clone(v->u.array.items[i]);
      if (!c || json_array_push(a, c)) {
        json_free(c);
        json_free(a);
        return 0;
      }
    }
    return a;
  }
  case JSON_OBJECT: {
    json_value *o = json_new_object();
    if (!o)
      return 0;
    for (i = 0; i < v->u.object.count; i++) {
      json_value *c = json_clone(v->u.object.vals[i]);
      if (!c || json_object_set(o, v->u.object.keys[i], c)) {
        json_free(c);
        json_free(o);
        return 0;
      }
    }
    return o;
  }
  }
  return 0;
}

/* ------------------------------------------------------------------ encode */

static int num_finite(double d)
{ /* false for NaN/+-Inf (exp all-ones) */
  union {
    double d;
    unsigned long long u;
  } x;
  x.d = d;
  return ((unsigned)(x.u >> 52) & 0x7ff) != 0x7ff;
}

static int enc_string(sbuf *s, const char *p, int len)
{
  static const char hex[] = "0123456789abcdef";
  int i;
  if (sb_putc(s, '"'))
    return -1;
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)p[i];
    int rc;
    switch (c) {
    case '"':
      rc = sb_put(s, "\\\"", 2);
      break;
    case '\\':
      rc = sb_put(s, "\\\\", 2);
      break;
    case '\b':
      rc = sb_put(s, "\\b", 2);
      break;
    case '\f':
      rc = sb_put(s, "\\f", 2);
      break;
    case '\n':
      rc = sb_put(s, "\\n", 2);
      break;
    case '\r':
      rc = sb_put(s, "\\r", 2);
      break;
    case '\t':
      rc = sb_put(s, "\\t", 2);
      break;
    default:
      if (c < 0x20) {
        char h[6];
        h[0] = '\\';
        h[1] = 'u';
        h[2] = '0';
        h[3] = '0';
        h[4] = hex[c >> 4];
        h[5] = hex[c & 15];
        rc = sb_put(s, h, 6);
      } else {
        rc = sb_putc(s, (char)c); /* UTF-8 bytes pass through unescaped */
      }
    }
    if (rc)
      return -1;
  }
  return sb_putc(s, '"');
}

static int enc_newline(sbuf *s, int indent, int depth)
{
  int i;
  if (!indent)
    return 0;
  if (sb_putc(s, '\n'))
    return -1;
  for (i = 0; i < indent * depth; i++)
    if (sb_putc(s, ' '))
      return -1;
  return 0;
}

static int enc_val(sbuf *s, const json_value *v, int indent, int depth)
{
  char tmp[DTOA_MAX];
  int i;
  if (!v)
    return sb_put(s, "null", 4);
  switch (v->type) {
  case JSON_NULL:
    return sb_put(s, "null", 4);
  case JSON_BOOL:
    return v->u.boolean ? sb_put(s, "true", 4) : sb_put(s, "false", 5);
  case JSON_NUMBER:
    if (!num_finite(v->u.number))
      return sb_put(s, "null", 4); /* JSON has no NaN/Inf */
    dtoa(v->u.number, tmp);
    return sb_puts(s, tmp);
  case JSON_STRING:
    return enc_string(s, v->u.string.data, v->u.string.len);
  case JSON_ARRAY:
    if (v->u.array.count == 0)
      return sb_put(s, "[]", 2);
    if (sb_putc(s, '['))
      return -1;
    for (i = 0; i < v->u.array.count; i++) {
      if (i && sb_putc(s, ','))
        return -1;
      if (enc_newline(s, indent, depth + 1))
        return -1;
      if (enc_val(s, v->u.array.items[i], indent, depth + 1))
        return -1;
    }
    if (enc_newline(s, indent, depth))
      return -1;
    return sb_putc(s, ']');
  case JSON_OBJECT:
    if (v->u.object.count == 0)
      return sb_put(s, "{}", 2);
    if (sb_putc(s, '{'))
      return -1;
    for (i = 0; i < v->u.object.count; i++) {
      if (i && sb_putc(s, ','))
        return -1;
      if (enc_newline(s, indent, depth + 1))
        return -1;
      if (enc_string(s, v->u.object.keys[i], (int)strlen(v->u.object.keys[i])))
        return -1;
      if (sb_putc(s, ':'))
        return -1;
      if (indent && sb_putc(s, ' '))
        return -1;
      if (enc_val(s, v->u.object.vals[i], indent, depth + 1))
        return -1;
    }
    if (enc_newline(s, indent, depth))
      return -1;
    return sb_putc(s, '}');
  }
  return -1;
}

char *json_encode(const json_value *v, int indent, int *out_len)
{
  sbuf s;
  s.b = 0;
  s.n = 0;
  s.cap = 0;
  if (indent < 0)
    indent = 0;
  if (enc_val(&s, v, indent, 0)) {
    free(s.b);
    return 0;
  }
  if (sb_putc(&s, 0)) {
    free(s.b);
    return 0;
  } /* NUL-terminate */
  if (out_len)
    *out_len = s.n - 1;
  return s.b;
}

/* ------------------------------------------------------------------ parser */

typedef struct {
  const char *s;
  int len, pos, depth;
  json_error *err;
} parser;

static json_value *parse_value(parser *p);
static json_value *parse_array(parser *p);
static json_value *parse_object(parser *p);

static void set_err(parser *p, const char *msg)
{
  int i, line = 1, col = 1, j = 0;
  if (!p->err)
    return;
  for (i = 0; i < p->pos && i < p->len; i++) {
    if (p->s[i] == '\n') {
      line++;
      col = 1;
    } else
      col++;
  }
  p->err->pos = p->pos;
  p->err->line = line;
  p->err->col = col;
  while (msg[j] && j < (int)sizeof p->err->msg - 1) {
    p->err->msg[j] = msg[j];
    j++;
  }
  p->err->msg[j] = 0;
}

static void skip_ws(parser *p)
{
  while (p->pos < p->len) {
    char c = p->s[p->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      p->pos++;
    else
      break;
  }
}

static int hex4(parser *p, unsigned *out)
{
  unsigned cp = 0;
  int i;
  for (i = 0; i < 4; i++) {
    char h;
    if (p->pos >= p->len)
      return -1;
    h = p->s[p->pos++];
    cp <<= 4;
    if (h >= '0' && h <= '9')
      cp |= (unsigned)(h - '0');
    else if (h >= 'a' && h <= 'f')
      cp |= (unsigned)(h - 'a' + 10);
    else if (h >= 'A' && h <= 'F')
      cp |= (unsigned)(h - 'A' + 10);
    else
      return -1;
  }
  *out = cp;
  return 0;
}

static int utf8_put(sbuf *s, unsigned cp)
{
  char b[4];
  if (cp < 0x80)
    return sb_putc(s, (char)cp);
  if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    return sb_put(s, b, 2);
  }
  if (cp < 0x10000) {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    return sb_put(s, b, 3);
  }
  b[0] = (char)(0xF0 | (cp >> 18));
  b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  b[3] = (char)(0x80 | (cp & 0x3F));
  return sb_put(s, b, 4);
}

/* On entry p->pos is at the opening quote. Returns a malloc'd NUL-terminated
 * buffer (with *out_len excluding the NUL), or NULL with the error set. */
static char *parse_string(parser *p, int *out_len)
{
  sbuf s;
  s.b = 0;
  s.n = 0;
  s.cap = 0;
  p->pos++; /* opening quote */
  for (;;) {
    unsigned char c;
    if (p->pos >= p->len) {
      set_err(p, "unterminated string");
      free(s.b);
      return 0;
    }
    c = (unsigned char)p->s[p->pos++];
    if (c == '"')
      break;
    if (c == '\\') {
      char e;
      int rc = 0;
      if (p->pos >= p->len) {
        set_err(p, "unterminated escape");
        free(s.b);
        return 0;
      }
      e = p->s[p->pos++];
      switch (e) {
      case '"':
        rc = sb_putc(&s, '"');
        break;
      case '\\':
        rc = sb_putc(&s, '\\');
        break;
      case '/':
        rc = sb_putc(&s, '/');
        break;
      case 'b':
        rc = sb_putc(&s, '\b');
        break;
      case 'f':
        rc = sb_putc(&s, '\f');
        break;
      case 'n':
        rc = sb_putc(&s, '\n');
        break;
      case 'r':
        rc = sb_putc(&s, '\r');
        break;
      case 't':
        rc = sb_putc(&s, '\t');
        break;
      case 'u': {
        unsigned cp;
        if (hex4(p, &cp)) {
          set_err(p, "invalid \\u escape");
          free(s.b);
          return 0;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && /* combine a surrogate pair */
            p->pos + 1 < p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
          unsigned lo;
          p->pos += 2;
          if (hex4(p, &lo)) {
            set_err(p, "invalid \\u escape");
            free(s.b);
            return 0;
          }
          if (lo >= 0xDC00 && lo <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          else {
            if (utf8_put(&s, cp)) {
              free(s.b);
              return 0;
            }
            cp = lo;
          } /* unpaired: emit both */
        }
        rc = utf8_put(&s, cp);
        break;
      }
      default:
        set_err(p, "invalid escape");
        free(s.b);
        return 0;
      }
      if (rc) {
        free(s.b);
        return 0;
      }
    } else if (c < 0x20) {
      set_err(p, "control character in string");
      free(s.b);
      return 0;
    } else if (sb_putc(&s, (char)c)) {
      free(s.b);
      return 0;
    }
  }
  if (sb_putc(&s, 0)) {
    free(s.b);
    return 0;
  } /* NUL-terminate (also handles "") */
  if (out_len)
    *out_len = s.n - 1;
  return s.b;
}

static json_value *parse_number(parser *p)
{
  int start = p->pos, n;
  char buf[340];
  if (p->pos < p->len && p->s[p->pos] == '-')
    p->pos++;
  if (p->pos >= p->len) {
    set_err(p, "invalid number");
    return 0;
  }
  if (p->s[p->pos] == '0')
    p->pos++;
  else if (p->s[p->pos] >= '1' && p->s[p->pos] <= '9')
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9')
      p->pos++;
  else {
    set_err(p, "invalid number");
    return 0;
  }

  if (p->pos < p->len && p->s[p->pos] == '.') {
    p->pos++;
    if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
      set_err(p, "invalid number");
      return 0;
    }
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9')
      p->pos++;
  }
  if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
    p->pos++;
    if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
      p->pos++;
    if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
      set_err(p, "invalid number");
      return 0;
    }
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9')
      p->pos++;
  }
  n = p->pos - start;
  if (n > (int)sizeof buf - 1) {
    set_err(p, "number too long");
    return 0;
  }
  memcpy(buf, p->s + start, (unsigned)n);
  buf[n] = 0;
  return json_new_number(strtod(buf, 0));
}

static json_value *parse_array(parser *p)
{
  json_value *arr;
  p->pos++; /* [ */
  if (++p->depth > JSON_MAX_DEPTH) {
    set_err(p, "max nesting depth exceeded");
    p->depth--;
    return 0;
  }
  arr = json_new_array();
  if (!arr) {
    set_err(p, "out of memory");
    p->depth--;
    return 0;
  }
  skip_ws(p);
  if (p->pos < p->len && p->s[p->pos] == ']') {
    p->pos++;
    p->depth--;
    return arr;
  }
  for (;;) {
    char c;
    json_value *v = parse_value(p);
    if (!v) {
      json_free(arr);
      p->depth--;
      return 0;
    }
    if (json_array_push(arr, v)) {
      set_err(p, "out of memory");
      json_free(v);
      json_free(arr);
      p->depth--;
      return 0;
    }
    skip_ws(p);
    if (p->pos >= p->len) {
      set_err(p, "unterminated array");
      json_free(arr);
      p->depth--;
      return 0;
    }
    c = p->s[p->pos++];
    if (c == ',')
      continue;
    if (c == ']')
      break;
    set_err(p, "expected ',' or ']'");
    json_free(arr);
    p->depth--;
    return 0;
  }
  p->depth--;
  return arr;
}

static json_value *parse_object(parser *p)
{
  json_value *obj;
  p->pos++; /* { */
  if (++p->depth > JSON_MAX_DEPTH) {
    set_err(p, "max nesting depth exceeded");
    p->depth--;
    return 0;
  }
  obj = json_new_object();
  if (!obj) {
    set_err(p, "out of memory");
    p->depth--;
    return 0;
  }
  skip_ws(p);
  if (p->pos < p->len && p->s[p->pos] == '}') {
    p->pos++;
    p->depth--;
    return obj;
  }
  for (;;) {
    char *key, c;
    int klen;
    json_value *v;
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != '"') {
      set_err(p, "expected string key");
      json_free(obj);
      p->depth--;
      return 0;
    }
    key = parse_string(p, &klen);
    if (!key) {
      json_free(obj);
      p->depth--;
      return 0;
    }
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != ':') {
      set_err(p, "expected ':'");
      free(key);
      json_free(obj);
      p->depth--;
      return 0;
    }
    p->pos++;
    v = parse_value(p);
    if (!v) {
      free(key);
      json_free(obj);
      p->depth--;
      return 0;
    }
    if (json_object_set(obj, key, v)) {
      set_err(p, "out of memory");
      free(key);
      json_free(v);
      json_free(obj);
      p->depth--;
      return 0;
    }
    free(key);
    skip_ws(p);
    if (p->pos >= p->len) {
      set_err(p, "unterminated object");
      json_free(obj);
      p->depth--;
      return 0;
    }
    c = p->s[p->pos++];
    if (c == ',')
      continue;
    if (c == '}')
      break;
    set_err(p, "expected ',' or '}'");
    json_free(obj);
    p->depth--;
    return 0;
  }
  p->depth--;
  return obj;
}

static json_value *parse_value(parser *p)
{
  char c;
  skip_ws(p);
  if (p->pos >= p->len) {
    set_err(p, "unexpected end of input");
    return 0;
  }
  c = p->s[p->pos];
  switch (c) {
  case '{':
    return parse_object(p);
  case '[':
    return parse_array(p);
  case '"': {
    int len;
    char *str = parse_string(p, &len);
    json_value *v;
    if (!str)
      return 0;
    v = val_new(JSON_STRING);
    if (!v) {
      set_err(p, "out of memory");
      free(str);
      return 0;
    }
    v->u.string.data = str;
    v->u.string.len = len;
    return v;
  }
  case 't':
    if (p->pos + 4 <= p->len && p->s[p->pos + 1] == 'r' && p->s[p->pos + 2] == 'u' && p->s[p->pos + 3] == 'e') {
      p->pos += 4;
      return json_new_bool(1);
    }
    set_err(p, "invalid literal");
    return 0;
  case 'f':
    if (p->pos + 5 <= p->len && p->s[p->pos + 1] == 'a' && p->s[p->pos + 2] == 'l' && p->s[p->pos + 3] == 's' &&
        p->s[p->pos + 4] == 'e') {
      p->pos += 5;
      return json_new_bool(0);
    }
    set_err(p, "invalid literal");
    return 0;
  case 'n':
    if (p->pos + 4 <= p->len && p->s[p->pos + 1] == 'u' && p->s[p->pos + 2] == 'l' && p->s[p->pos + 3] == 'l') {
      p->pos += 4;
      return json_new_null();
    }
    set_err(p, "invalid literal");
    return 0;
  default:
    if (c == '-' || (c >= '0' && c <= '9'))
      return parse_number(p);
    set_err(p, "unexpected character");
    return 0;
  }
}

json_value *json_parse(const char *text, int len, json_error *err)
{
  parser p;
  json_value *v;
  if (err) {
    err->pos = 0;
    err->line = 1;
    err->col = 1;
    err->msg[0] = 0;
  }
  if (!text) {
    if (err)
      strcpy(err->msg, "null input");
    return 0;
  }
  p.s = text;
  p.len = len < 0 ? (int)strlen(text) : len;
  p.pos = 0;
  p.depth = 0;
  p.err = err;
  v = parse_value(&p);
  if (!v)
    return 0;
  skip_ws(&p);
  if (p.pos != p.len) {
    set_err(&p, "trailing characters after value");
    json_free(v);
    return 0;
  }
  return v;
}

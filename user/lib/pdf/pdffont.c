/* pdffont.c, PDF fonts: encodings, widths, and the glyph a character code draws */
#include "owos.h"
#include "pdfint.h"

struct range {
  int lo, hi;
  double w;
};

struct pfont {
  struct pfont *next;
  int num;
  const void *kv;
  int composite, type3;
  const struct font *face;
  struct font *own;
  unsigned char *own_data;
  int embedded, symbolic;
  unsigned short uni[256];
  float w[256];
  double missing, dw;
  unsigned short *cid2gid;
  int ncid2gid;
  struct range *cw;
  int ncw;
  struct range *tu; /* ToUnicode: codes lo..hi map to w.. consecutively */
  int ntu;
};

static const char *const ascii_names[95] = {"space",       "exclam",
                                            "quotedbl",    "numbersign",
                                            "dollar",      "percent",
                                            "ampersand",   "quotesingle",
                                            "parenleft",   "parenright",
                                            "asterisk",    "plus",
                                            "comma",       "hyphen",
                                            "period",      "slash",
                                            "zero",        "one",
                                            "two",         "three",
                                            "four",        "five",
                                            "six",         "seven",
                                            "eight",       "nine",
                                            "colon",       "semicolon",
                                            "less",        "equal",
                                            "greater",     "question",
                                            "at",          "A",
                                            "B",           "C",
                                            "D",           "E",
                                            "F",           "G",
                                            "H",           "I",
                                            "J",           "K",
                                            "L",           "M",
                                            "N",           "O",
                                            "P",           "Q",
                                            "R",           "S",
                                            "T",           "U",
                                            "V",           "W",
                                            "X",           "Y",
                                            "Z",           "bracketleft",
                                            "backslash",   "bracketright",
                                            "asciicircum", "underscore",
                                            "grave",       "a",
                                            "b",           "c",
                                            "d",           "e",
                                            "f",           "g",
                                            "h",           "i",
                                            "j",           "k",
                                            "l",           "m",
                                            "n",           "o",
                                            "p",           "q",
                                            "r",           "s",
                                            "t",           "u",
                                            "v",           "w",
                                            "x",           "y",
                                            "z",           "braceleft",
                                            "bar",         "braceright",
                                            "asciitilde"};

static const char *const latin1_a1[31] = {
    "exclamdown", "cent",           "sterling",    "currency",      "yen",           "brokenbar",      "section",
    "dieresis",   "copyright",      "ordfeminine", "guillemotleft", "logicalnot",    "sfthyphen",      "registered",
    "macron",     "degree",         "plusminus",   "twosuperior",   "threesuperior", "acute",          "mu",
    "paragraph",  "periodcentered", "cedilla",     "onesuperior",   "ordmasculine",  "guillemotright", "onequarter",
    "onehalf",    "threequarters",  "questiondown"};

static const char *const latin1_c0[64] = {
    "Agrave", "Aacute", "Acircumflex", "Atilde",      "Adieresis",   "Aring",  "AE",          "Ccedilla",
    "Egrave", "Eacute", "Ecircumflex", "Edieresis",   "Igrave",      "Iacute", "Icircumflex", "Idieresis",
    "Eth",    "Ntilde", "Ograve",      "Oacute",      "Ocircumflex", "Otilde", "Odieresis",   "multiply",
    "Oslash", "Ugrave", "Uacute",      "Ucircumflex", "Udieresis",   "Yacute", "Thorn",       "germandbls",
    "agrave", "aacute", "acircumflex", "atilde",      "adieresis",   "aring",  "ae",          "ccedilla",
    "egrave", "eacute", "ecircumflex", "edieresis",   "igrave",      "iacute", "icircumflex", "idieresis",
    "eth",    "ntilde", "ograve",      "oacute",      "ocircumflex", "otilde", "odieresis",   "divide",
    "oslash", "ugrave", "uacute",      "ucircumflex", "udieresis",   "yacute", "thorn",       "ydieresis"};

static const struct {
  const char *n;
  unsigned short u;
} extra_names[] = {
    {"nbspace", 0xA0},
    {"hyphen", 0x2D},
    {"quoteleft", 0x2018},
    {"quoteright", 0x2019},
    {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D},
    {"quotesinglbase", 0x201A},
    {"quotedblbase", 0x201E},
    {"endash", 0x2013},
    {"emdash", 0x2014},
    {"bullet", 0x2022},
    {"ellipsis", 0x2026},
    {"dagger", 0x2020},
    {"daggerdbl", 0x2021},
    {"perthousand", 0x2030},
    {"guilsinglleft", 0x2039},
    {"guilsinglright", 0x203A},
    {"fi", 0xFB01},
    {"fl", 0xFB02},
    {"ff", 0xFB00},
    {"ffi", 0xFB03},
    {"ffl", 0xFB04},
    {"Euro", 0x20AC},
    {"trademark", 0x2122},
    {"minus", 0x2212},
    {"fraction", 0x2044},
    {"florin", 0x192},
    {"OE", 0x152},
    {"oe", 0x153},
    {"Scaron", 0x160},
    {"scaron", 0x161},
    {"Zcaron", 0x17D},
    {"zcaron", 0x17E},
    {"Ydieresis", 0x178},
    {"circumflex", 0x2C6},
    {"tilde", 0x2DC},
    {"ring", 0x2DA},
    {"caron", 0x2C7},
    {"breve", 0x2D8},
    {"dotaccent", 0x2D9},
    {"hungarumlaut", 0x2DD},
    {"ogonek", 0x2DB},
    {"dotlessi", 0x131},
    {"Lslash", 0x141},
    {"lslash", 0x142},
    {"mu1", 0xB5},
    {"Delta", 0x394},
    {"Omega", 0x3A9},
    {"pi", 0x3C0},
    {"alpha", 0x3B1},
    {"beta", 0x3B2},
    {"gamma", 0x3B3},
    {"lambda", 0x3BB},
    {"sigma", 0x3C3},
    {"summation", 0x2211},
    {"radical", 0x221A},
    {"infinity", 0x221E},
    {"notequal", 0x2260},
    {"lessequal", 0x2264},
    {"greaterequal", 0x2265},
    {"approxequal", 0x2248},
    {"arrowright", 0x2192},
    {"arrowleft", 0x2190},
    {"arrowup", 0x2191},
    {"arrowdown", 0x2193},
    {"periodcentered", 0xB7},
    {"middot", 0xB7},
    {"Scedilla", 0x15E},
    {"scedilla", 0x15F},
    {"Gbreve", 0x11E},
    {"gbreve", 0x11F},
    {"Idotaccent", 0x130},
    {"Cacute", 0x106},
    {"cacute", 0x107},
    {"Ccaron", 0x10C},
    {"ccaron", 0x10D},
    {"Dcaron", 0x10E},
    {"dcaron", 0x10F},
    {"Ecaron", 0x11A},
    {"ecaron", 0x11B},
    {"Lcaron", 0x13D},
    {"lcaron", 0x13E},
    {"Nacute", 0x143},
    {"nacute", 0x144},
    {"Ncaron", 0x147},
    {"ncaron", 0x148},
    {"Rcaron", 0x158},
    {"rcaron", 0x159},
    {"Sacute", 0x15A},
    {"sacute", 0x15B},
    {"Tcaron", 0x164},
    {"tcaron", 0x165},
    {"Uring", 0x16E},
    {"uring", 0x16F},
    {"Zacute", 0x179},
    {"zacute", 0x17A},
    {"Zdotaccent", 0x17B},
    {"zdotaccent", 0x17C},
    {"Aogonek", 0x104},
    {"aogonek", 0x105},
    {"Eogonek", 0x118},
    {"eogonek", 0x119},
    {"Racute", 0x154},
    {"racute", 0x155},
};

static const unsigned short win_80[32] = {0x20AC, 0x2022, 0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021,
                                          0x2C6,  0x2030, 0x160,  0x2039, 0x152,  0x2022, 0x17D,  0x2022,
                                          0x2022, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                          0x2DC,  0x2122, 0x161,  0x203A, 0x153,  0x2022, 0x17E,  0x178};

static const unsigned short mac_80[128] = {
    0xC4,  0xC5,   0xC7,   0xC9,   0xD1,   0xD6,   0xDC,   0xE1,   0xE0,   0xE2,   0xE4, 0xE3,   0xE5,
    0xE7,  0xE9,   0xE8,   0xEA,   0xEB,   0xED,   0xEC,   0xEE,   0xEF,   0xF1,   0xF3, 0xF2,   0xF4,
    0xF6,  0xF5,   0xFA,   0xF9,   0xFB,   0xFC,   0x2020, 0xB0,   0xA2,   0xA3,   0xA7, 0x2022, 0xB6,
    0xDF,  0xAE,   0xA9,   0x2122, 0xB4,   0xA8,   0x2260, 0xC6,   0xD8,   0x221E, 0xB1, 0x2264, 0x2265,
    0xA5,  0xB5,   0x2202, 0x2211, 0x220F, 0x3C0,  0x222B, 0xAA,   0xBA,   0x3A9,  0xE6, 0xF8,   0xBF,
    0xA1,  0xAC,   0x221A, 0x192,  0x2248, 0x2206, 0xAB,   0xBB,   0x2026, 0xA0,   0xC0, 0xC3,   0xD5,
    0x152, 0x153,  0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7,   0x25CA, 0xFF, 0x178,  0x2044,
    0xA4,  0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0xB7,   0x201A, 0x201E, 0x2030, 0xC2, 0xCA,   0xC1,
    0xCB,  0xC8,   0xCD,   0xCE,   0xCF,   0xCC,   0xD3,   0xD4,   0,      0xD2,   0xDA, 0xDB,   0xD9,
    0x131, 0x2C6,  0x2DC,  0xAF,   0x2D8,  0x2D9,  0x2DA,  0xB8,   0x2DD,  0x2DB,  0x2C7};

static const struct {
  unsigned char c;
  unsigned short u;
} std_hi[] = {{0xA1, 0xA1},   {0xA2, 0xA2},   {0xA3, 0xA3},   {0xA4, 0x2044}, {0xA5, 0xA5},   {0xA6, 0x192},
              {0xA7, 0xA7},   {0xA8, 0xA4},   {0xA9, 0x27},   {0xAA, 0x201C}, {0xAB, 0xAB},   {0xAC, 0x2039},
              {0xAD, 0x203A}, {0xAE, 0xFB01}, {0xAF, 0xFB02}, {0xB1, 0x2013}, {0xB2, 0x2020}, {0xB3, 0x2021},
              {0xB4, 0xB7},   {0xB6, 0xB6},   {0xB7, 0x2022}, {0xB8, 0x201A}, {0xB9, 0x201E}, {0xBA, 0x201D},
              {0xBB, 0xBB},   {0xBC, 0x2026}, {0xBD, 0x2030}, {0xBF, 0xBF},   {0xC1, 0x60},   {0xC2, 0xB4},
              {0xC3, 0x2C6},  {0xC4, 0x2DC},  {0xC5, 0xAF},   {0xC6, 0x2D8},  {0xC7, 0x2D9},  {0xC8, 0xA8},
              {0xCA, 0x2DA},  {0xCB, 0xB8},   {0xCD, 0x2DD},  {0xCE, 0x2DB},  {0xCF, 0x2C7},  {0xD0, 0x2014},
              {0xE1, 0xC6},   {0xE3, 0xAA},   {0xE8, 0x141},  {0xE9, 0xD8},   {0xEA, 0x152},  {0xEB, 0xBA},
              {0xF1, 0xE6},   {0xF5, 0x131},  {0xF8, 0x142},  {0xF9, 0xF8},   {0xFA, 0x153},  {0xFB, 0xDF}};

static int hexv(int c)
{
  return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

static int glyph_unicode(const char *n)
{
  if (!n[0])
    return 0;
  if (!n[1])
    return (unsigned char)n[0];
  for (int i = 0; i < 95; i++)
    if (!strcmp(n, ascii_names[i]))
      return 0x20 + i;
  for (int i = 0; i < 31; i++)
    if (!strcmp(n, latin1_a1[i]))
      return 0xA1 + i;
  for (int i = 0; i < 64; i++)
    if (!strcmp(n, latin1_c0[i]))
      return 0xC0 + i;
  for (unsigned i = 0; i < sizeof extra_names / sizeof *extra_names; i++)
    if (!strcmp(n, extra_names[i].n))
      return extra_names[i].u;
  const char *h = n[0] == 'u' && n[1] == 'n' && n[2] == 'i' ? n + 3 : n[0] == 'u' && hexv(n[1]) >= 0 ? n + 1 : 0;
  if (h) {
    int v = 0, k = 0;
    for (; k < 6 && hexv(h[k]) >= 0; k++)
      v = v * 16 + hexv(h[k]);
    if (k >= 4)
      return v;
  }
  return 0;
}

/* The Macintosh Roman code for Unicode `u`, else `dflt` */
static int mac_code(int u, int dflt)
{
  if (u > 0 && u < 128)
    return u;
  for (int i = 0; i < 128; i++)
    if (u && mac_80[i] == u)
      return 128 + i;
  return dflt;
}

static void base_encoding(struct pfont *pf, const char *name)
{
  int mac = !strcmp(name, "MacRomanEncoding"), std = !strcmp(name, "StandardEncoding");
  for (int c = 0; c < 256; c++) {
    int u;
    if (c < 32)
      u = 0;
    else if (c < 127)
      u = c;
    else if (mac)
      u = c >= 128 ? mac_80[c - 128] : 0;
    else if (std)
      u = 0;
    else
      u = c >= 0x80 && c < 0xA0 ? win_80[c - 0x80] : c == 0xA0 ? 0x20 : c >= 0xA1 ? c : 0;
    pf->uni[c] = (unsigned short)u;
  }
  if (std) {
    pf->uni[0x27] = 0x2019;
    pf->uni[0x60] = 0x2018;
    for (unsigned i = 0; i < sizeof std_hi / sizeof *std_hi; i++)
      pf->uni[std_hi[i].c] = std_hi[i].u;
  }
}

static void tounicode(struct pdf *d, struct pfont *pf, struct pobj tu)
{
  int len = 0;
  unsigned char *data = tu.t == PO_STREAM ? pdf_stream_data(d, tu, &len, 0, 0) : 0;
  if (!data)
    return;
  struct arena ar = {0};
  struct lex l = {data, len, 0, &ar, 0};
  struct pobj t, a, b, c;
  int cap = 0;
  while (pdf_lex(&l, &t)) {
    int range = t.t == PO_KW && !strcmp(t.u.s, "beginbfrange"), chr = t.t == PO_KW && !strcmp(t.u.s, "beginbfchar");
    if (!range && !chr)
      continue;
    while (pdf_lex(&l, &a)) {
      if (a.t != PO_STR)
        break;
      int lo = 0, hi, dst = 0;
      for (int i = 0; i < a.n; i++)
        lo = lo << 8 | (unsigned char)a.u.s[i];
      hi = lo;
      if (range) {
        if (!pdf_lex(&l, &b) || b.t != PO_STR)
          break;
        hi = 0;
        for (int i = 0; i < b.n; i++)
          hi = hi << 8 | (unsigned char)b.u.s[i];
      }
      if (!pdf_lex(&l, &c))
        break;
      struct pobj first = c;
      if (c.t == PO_ARR && c.n > 0)
        first = c.u.a[0];
      if (first.t == PO_STR && first.n >= 2) {
        dst = (unsigned char)first.u.s[0] << 8 | (unsigned char)first.u.s[1];
        if (dst >= 0xD800 && dst < 0xDC00 && first.n >= 4)
          dst = 0x10000 + ((dst - 0xD800) << 10) +
                (((unsigned char)first.u.s[2] << 8 | (unsigned char)first.u.s[3]) - 0xDC00);
      } else if (first.t == PO_STR && first.n == 1)
        dst = (unsigned char)first.u.s[0];
      int n = c.t == PO_ARR && range ? c.n : 1;
      for (int i = 0; i < n && i < 65536; i++) {
        if (pf->ntu == cap) {
          cap = cap ? cap * 2 : 256;
          struct range *nr = realloc(pf->tu, (unsigned)cap * sizeof *nr);
          if (!nr)
            break;
          pf->tu = nr;
        }
        int u = dst;
        if (c.t == PO_ARR && c.u.a[i].t == PO_STR && c.u.a[i].n >= 2)
          u = (unsigned char)c.u.a[i].u.s[0] << 8 | (unsigned char)c.u.a[i].u.s[1];
        pf->tu[pf->ntu++] = c.t == PO_ARR ? (struct range){lo + i, lo + i, u} : (struct range){lo, hi, u};
      }
    }
  }
  arena_free(&ar);
  free(data);
}

static int tounicode_lookup(const struct pfont *pf, int code)
{
  for (int i = 0; i < pf->ntu; i++)
    if (code >= pf->tu[i].lo && code <= pf->tu[i].hi)
      return (int)pf->tu[i].w + code - pf->tu[i].lo;
  return 0;
}

static int has_word(const char *s, const char *w)
{
  int n = strlen(w);
  for (; *s; s++) {
    int i = 0;
    while (i < n && s[i] && (s[i] | 32) == (w[i] | 32))
      i++;
    if (i == n)
      return 1;
  }
  return 0;
}

static void load_program(struct pdf *d, struct pfont *pf, struct pobj desc)
{
  struct pobj ff = pdf_get(d, desc, "FontFile2");
  if (ff.t != PO_STREAM)
    ff = pdf_get(d, desc, "FontFile3");
  if (ff.t != PO_STREAM)
    return;
  int len = 0;
  const char *left = 0;
  unsigned char *data = pdf_stream_data(d, ff, &len, &left, 0);
  if (!data)
    return;
  struct font *f = left ? 0 : font_load(data, len);
  if (!f) {
    free(data);
    return;
  }
  pf->own = f;
  pf->own_data = data;
  pf->face = f;
  pf->embedded = 1;
}

static void pick_fallback(struct pdf *d, struct pfont *pf, struct pobj fd, struct pobj desc)
{
  if (pf->face)
    return;
  struct pobj bf = pdf_get(d, fd, "BaseFont");
  int flags = (int)pdf_num(pdf_get(d, desc, "Flags"), 0);
  int mono = (flags & 1) || (bf.t == PO_NAME && (has_word(bf.u.s, "Courier") || has_word(bf.u.s, "Mono") ||
                                                 has_word(bf.u.s, "Consolas") || has_word(bf.u.s, "Typewriter")));
  pf->face = mono ? (d->mono ? d->mono : d->sans) : (d->sans ? d->sans : d->mono);
}

static void parse_cid_widths(struct pdf *d, struct pfont *pf, struct pobj w)
{
  int cap = 0;
  for (int i = 0; i < w.n;) {
    struct pobj a = pdf_at(d, w, i), b = pdf_at(d, w, i + 1);
    if (a.t != PO_INT && a.t != PO_REAL)
      break;
    int c = (int)pdf_num(a, 0);
    if (b.t == PO_ARR) {
      for (int k = 0; k < b.n; k++) {
        if (pf->ncw == cap) {
          cap = cap ? cap * 2 : 128;
          struct range *nr = realloc(pf->cw, (unsigned)cap * sizeof *nr);
          if (!nr)
            return;
          pf->cw = nr;
        }
        pf->cw[pf->ncw++] = (struct range){c + k, c + k, pdf_num(pdf_at(d, b, k), 0)};
      }
      i += 2;
    } else {
      if (pf->ncw == cap) {
        cap = cap ? cap * 2 : 128;
        struct range *nr = realloc(pf->cw, (unsigned)cap * sizeof *nr);
        if (!nr)
          return;
        pf->cw = nr;
      }
      pf->cw[pf->ncw++] = (struct range){c, (int)pdf_num(b, c), pdf_num(pdf_at(d, w, i + 2), 0)};
      i += 3;
    }
  }
}

static void load_composite(struct pdf *d, struct pfont *pf, struct pobj fd)
{
  pf->composite = 1;
  struct pobj kids = pdf_get(d, fd, "DescendantFonts");
  struct pobj cf = kids.t == PO_ARR ? pdf_at(d, kids, 0) : kids;
  struct pobj desc = pdf_get(d, cf, "FontDescriptor");
  pf->dw = pdf_num(pdf_get(d, cf, "DW"), 1000);
  parse_cid_widths(d, pf, pdf_get(d, cf, "W"));
  load_program(d, pf, desc);
  struct pobj map = pdf_get(d, cf, "CIDToGIDMap");
  if (map.t == PO_STREAM) {
    int len = 0;
    unsigned char *m = pdf_stream_data(d, map, &len, 0, 0);
    if (m) {
      pf->ncid2gid = len / 2;
      pf->cid2gid = malloc((unsigned)len + 2);
      for (int i = 0; pf->cid2gid && i < pf->ncid2gid; i++)
        pf->cid2gid[i] = (unsigned short)(m[i * 2] << 8 | m[i * 2 + 1]);
      free(m);
    }
  }
  tounicode(d, pf, pdf_get(d, fd, "ToUnicode"));
  pick_fallback(d, pf, cf, desc);
}

static void load_simple(struct pdf *d, struct pfont *pf, struct pobj fd, int truetype)
{
  struct pobj desc = pdf_get(d, fd, "FontDescriptor");
  int flags = (int)pdf_num(pdf_get(d, desc, "Flags"), 0);
  pf->symbolic = (flags & 4) && !(flags & 32);
  pf->missing = pdf_num(pdf_get(d, desc, "MissingWidth"), 0);
  load_program(d, pf, desc);
  pick_fallback(d, pf, fd, desc);

  struct pobj enc = pdf_get(d, fd, "Encoding");
  const char *base = truetype ? "WinAnsiEncoding" : "StandardEncoding";
  if (enc.t == PO_NAME)
    base = enc.u.s;
  else if (pdf_is_dict(enc)) {
    struct pobj be = pdf_get(d, enc, "BaseEncoding");
    if (be.t == PO_NAME)
      base = be.u.s;
  }
  base_encoding(pf, base);
  if (pdf_is_dict(enc)) {
    struct pobj diff = pdf_get(d, enc, "Differences");
    int code = 0;
    for (int i = 0; i < diff.n; i++) {
      struct pobj e = pdf_at(d, diff, i);
      if (e.t == PO_INT)
        code = e.n;
      else if (e.t == PO_NAME) {
        if (code >= 0 && code < 256)
          pf->uni[code] = (unsigned short)glyph_unicode(e.u.s);
        code++;
      }
    }
  }
  struct pobj ws = pdf_get(d, fd, "Widths");
  int first = (int)pdf_num(pdf_get(d, fd, "FirstChar"), 0);
  for (int c = 0; c < 256; c++)
    pf->w[c] = -1;
  double scale = 1;
  if (pf->type3) {
    struct pobj fm = pdf_get(d, fd, "FontMatrix");
    scale = pdf_num(pdf_at(d, fm, 0), 0.001) * 1000;
  }
  for (int i = 0; i < ws.n; i++)
    if (first + i >= 0 && first + i < 256)
      pf->w[first + i] = (float)(pdf_num(pdf_at(d, ws, i), 0) * scale);
}

struct pfont *pdf_font_load(struct pdf *d, struct pobj o)
{
  int num = o.t == PO_REF ? o.n : -1;
  struct pobj fd = pdf_deref(d, o);
  if (!pdf_is_dict(fd))
    return 0;
  const void *kv = fd.u.kv;
  for (struct pfont *f = d->fonts; f; f = f->next)
    if (f->num == num && f->kv == kv)
      return f;
  struct pfont *pf = calloc(1, sizeof *pf);
  if (!pf)
    return 0;
  pf->num = num;
  pf->kv = kv;
  struct pobj st = pdf_get(d, fd, "Subtype");
  if (pdf_name_is(st, "Type0"))
    load_composite(d, pf, fd);
  else {
    pf->type3 = pdf_name_is(st, "Type3");
    load_simple(d, pf, fd, pdf_name_is(st, "TrueType"));
  }
  pf->next = d->fonts;
  d->fonts = pf;
  return pf;
}

void pdf_fonts_free(struct pdf *d)
{
  while (d->fonts) {
    struct pfont *f = d->fonts;
    d->fonts = f->next;
    font_free(f->own);
    free(f->own_data);
    free(f->cid2gid);
    free(f->cw);
    free(f->tu);
    free(f);
  }
}

int pdf_font_next(struct pdf *d, struct pfont *pf, const unsigned char *s, int n, int *code, double *w,
                  struct glyph_ref *g, int *is_space)
{
  (void)d;
  g->f = 0;
  g->gid = 0;
  g->ok = 0;
  g->uni = 0;
  int used = pf->composite ? (n >= 2 ? 2 : 1) : 1;
  int c = used == 2 ? s[0] << 8 | s[1] : s[0];
  *code = c;
  *is_space = used == 1 && c == 32;
  int uni;
  if (pf->composite) {
    *w = pf->dw;
    g->uni = tounicode_lookup(pf, c);
    for (int i = 0; i < pf->ncw; i++)
      if (c >= pf->cw[i].lo && c <= pf->cw[i].hi) {
        *w = pf->cw[i].w;
        break;
      }
    if (pf->embedded) {
      g->f = pf->face;
      g->gid = pf->cid2gid ? (c < pf->ncid2gid ? pf->cid2gid[c] : 0) : c;
      g->ok = 1;
    } else if (pf->face && (uni = tounicode_lookup(pf, c)) != 0) {
      g->f = pf->face;
      g->gid = font_glyph_index(pf->face, uni);
      g->ok = g->gid != 0;
    }
    return used;
  }
  uni = pf->uni[c];
  g->uni = uni;
  if (pf->face && !pf->type3) {
    int gid = uni ? font_glyph_index(pf->face, uni) : 0;
    if (!gid && pf->embedded) {
      gid = font_glyph_index_enc(pf->face, 3, 0, c);
      if (!gid)
        gid = font_glyph_index_enc(pf->face, 3, 0, 0xF000 + c);
      if (!gid)
        gid = font_glyph_index_enc(pf->face, 1, 0, mac_code(uni, c));
      if (!gid)
        gid = font_glyph_index(pf->face, c);
    }
    g->f = pf->face;
    g->gid = gid;
    g->ok = gid != 0;
  }
  if (pf->w[c] >= 0)
    *w = pf->w[c];
  else if (pf->type3 || !g->ok)
    *w = pf->missing;
  else {
    int adv = 0, lsb = 0;
    font_glyph_hmetrics(g->f, g->gid, 1000.0f, &adv, &lsb);
    *w = adv;
  }
  return used;
}

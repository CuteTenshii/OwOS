#include "html_int.h"

/* decode_entity returns a real Unicode codepoint now (the renderer is UTF-8), or the sentinels */
int decode_entity(const char *p, const char *end, int *adv)
{
  int n = 1;
  while (p + n < end && n < 32 && p[n] != ';' && p[n] != '&' && p[n] != ' ' && p[n] != '<')
    n++;
  if (p + n >= end || p[n] != ';') {
    *adv = 1;
    return '&';
  }
  const char *body = p + 1;
  int blen = n - 1;
  if (blen >= 2 && body[0] == '#') {
    unsigned v = 0;
    if (body[1] == 'x' || body[1] == 'X') {
      for (int i = 2; i < blen; i++) {
        char c = body[i];
        if (c >= '0' && c <= '9')
          v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
          v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
          v = v * 16 + (c - 'A' + 10);
        else {
          v = 0;
          break;
        }
      }
    } else {
      for (int i = 1; i < blen; i++) {
        char c = body[i];
        if (c >= '0' && c <= '9')
          v = v * 10 + (c - '0');
        else {
          v = 0;
          break;
        }
      }
    }
    *adv = n + 1;
    if (v == 0)
      return -2;                            /* NULL -> drop      */
    if (v >= 0x80 && v <= 0x9F)             /* HTML5: C1 numeric */
      return cp1252_high((unsigned char)v); /* refs are cp1252   */
    if (v == 0xA0)
      return -1; /* nbsp -> coll. sp. */
    if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
      return 0xFFFD; /* invalid -> U+FFFD */
    return (int)v;
  }
  /* Named entities -> their real Unicode codepoints (the renderer is UTF-8) */
  static const struct {
    const char *name;
    int ch;
  } ent[] = {/* Markup essentials */
             {"amp", '&'},
             {"AMP", '&'},
             {"lt", '<'},
             {"LT", '<'},
             {"gt", '>'},
             {"GT", '>'},
             {"quot", '"'},
             {"QUOT", '"'},
             {"apos", '\''},
             /* Spaces and joiners */
             {"nbsp", -1},
             {"NonBreakingSpace", -1},
             {"ensp", 0x2002},
             {"emsp", 0x2003},
             {"emsp13", 0x2004},
             {"emsp14", 0x2005},
             {"numsp", 0x2007},
             {"puncsp", 0x2008},
             {"thinsp", 0x2009},
             {"ThinSpace", 0x2009},
             {"hairsp", 0x200A},
             {"VeryThinSpace", 0x200A},
             {"zwnj", 0x200C},
             {"zwj", 0x200D},
             {"lrm", 0x200E},
             {"rlm", 0x200F},
             {"shy", 0xAD},
             /* General punctuation */
             {"ndash", 0x2013},
             {"mdash", 0x2014},
             {"horbar", 0x2015},
             {"hellip", 0x2026},
             {"mldr", 0x2026},
             {"nldr", 0x2025},
             {"lsquo", 0x2018},
             {"rsquo", 0x2019},
             {"rsquor", 0x2019},
             {"lsquor", 0x201A},
             {"sbquo", 0x201A},
             {"ldquo", 0x201C},
             {"rdquo", 0x201D},
             {"rdquor", 0x201D},
             {"ldquor", 0x201E},
             {"bdquo", 0x201E},
             {"dagger", 0x2020},
             {"Dagger", 0x2021},
             {"ddagger", 0x2021},
             {"bull", 0x2022},
             {"bullet", 0x2022},
             {"tridot", 0x25EC},
             {"prime", 0x2032},
             {"Prime", 0x2033},
             {"tprime", 0x2034},
             {"bprime", 0x2035},
             {"backprime", 0x2035},
             {"lsaquo", 0x2039},
             {"rsaquo", 0x203A},
             {"oline", 0x203E},
             {"OverBar", 0x203E},
             {"caret", 0x2041},
             {"hybull", 0x2043},
             {"frasl", 0x2044},
             {"permil", 0x2030},
             {"pertenk", 0x2031},
             {"lowbar", '_'},
             {"verbar", '|'},
             {"vert", '|'},
             {"Verbar", 0x2016},
             {"commat", '@'},
             {"num", '#'},
             {"percnt", '%'},
             {"ast", '*'},
             {"midast", '*'},
             {"plus", '+'},
             {"sol", '/'},
             {"bsol", '\\'},
             {"colon", ':'},
             {"semi", ';'},
             {"quest", '?'},
             {"excl", '!'},
             {"period", '.'},
             {"comma", ','},
             {"lpar", '('},
             {"rpar", ')'},
             {"lbrack", '['},
             {"lsqb", '['},
             {"rbrack", ']'},
             {"rsqb", ']'},
             {"lbrace", '{'},
             {"lcub", '{'},
             {"rbrace", '}'},
             {"rcub", '}'},
             {"grave", '`'},
             {"DiacriticalGrave", '`'},
             {"Hat", '^'},
             {"dollar", '$'},
             {"equals", '='},
             /* Latin-1 symbols */
             {"iexcl", 0xA1},
             {"cent", 0xA2},
             {"pound", 0xA3},
             {"curren", 0xA4},
             {"yen", 0xA5},
             {"brvbar", 0xA6},
             {"sect", 0xA7},
             {"uml", 0xA8},
             {"die", 0xA8},
             {"Dot", 0xA8},
             {"copy", 0xA9},
             {"ordf", 0xAA},
             {"laquo", 0xAB},
             {"not", 0xAC},
             {"reg", 0xAE},
             {"circledR", 0xAE},
             {"macr", 0xAF},
             {"strns", 0xAF},
             {"deg", 0xB0},
             {"plusmn", 0xB1},
             {"pm", 0xB1},
             {"PlusMinus", 0xB1},
             {"sup2", 0xB2},
             {"sup3", 0xB3},
             {"acute", 0xB4},
             {"DiacriticalAcute", 0xB4},
             {"micro", 0xB5},
             {"para", 0xB6},
             {"middot", 0xB7},
             {"centerdot", 0xB7},
             {"CenterDot", 0xB7},
             {"cedil", 0xB8},
             {"Cedilla", 0xB8},
             {"sup1", 0xB9},
             {"ordm", 0xBA},
             {"raquo", 0xBB},
             {"frac14", 0xBC},
             {"frac12", 0xBD},
             {"half", 0xBD},
             {"frac34", 0xBE},
             {"iquest", 0xBF},
             {"times", 0xD7},
             {"divide", 0xF7},
             {"div", 0xF7},
             /* Latin-1 letters */
             {"Agrave", 0xC0},
             {"Aacute", 0xC1},
             {"Acirc", 0xC2},
             {"Atilde", 0xC3},
             {"Auml", 0xC4},
             {"Aring", 0xC5},
             {"AElig", 0xC6},
             {"Ccedil", 0xC7},
             {"Egrave", 0xC8},
             {"Eacute", 0xC9},
             {"Ecirc", 0xCA},
             {"Euml", 0xCB},
             {"Igrave", 0xCC},
             {"Iacute", 0xCD},
             {"Icirc", 0xCE},
             {"Iuml", 0xCF},
             {"ETH", 0xD0},
             {"Ntilde", 0xD1},
             {"Ograve", 0xD2},
             {"Oacute", 0xD3},
             {"Ocirc", 0xD4},
             {"Otilde", 0xD5},
             {"Ouml", 0xD6},
             {"Oslash", 0xD8},
             {"Ugrave", 0xD9},
             {"Uacute", 0xDA},
             {"Ucirc", 0xDB},
             {"Uuml", 0xDC},
             {"Yacute", 0xDD},
             {"THORN", 0xDE},
             {"szlig", 0xDF},
             {"agrave", 0xE0},
             {"aacute", 0xE1},
             {"acirc", 0xE2},
             {"atilde", 0xE3},
             {"auml", 0xE4},
             {"aring", 0xE5},
             {"aelig", 0xE6},
             {"ccedil", 0xE7},
             {"egrave", 0xE8},
             {"eacute", 0xE9},
             {"ecirc", 0xEA},
             {"euml", 0xEB},
             {"igrave", 0xEC},
             {"iacute", 0xED},
             {"icirc", 0xEE},
             {"iuml", 0xEF},
             {"eth", 0xF0},
             {"ntilde", 0xF1},
             {"ograve", 0xF2},
             {"oacute", 0xF3},
             {"ocirc", 0xF4},
             {"otilde", 0xF5},
             {"ouml", 0xF6},
             {"oslash", 0xF8},
             {"ugrave", 0xF9},
             {"uacute", 0xFA},
             {"ucirc", 0xFB},
             {"uuml", 0xFC},
             {"yacute", 0xFD},
             {"thorn", 0xFE},
             {"yuml", 0xFF},
             /* Latin Extended-A: ligatures and accented letters in common use */
             {"OElig", 0x152},
             {"oelig", 0x153},
             {"Scaron", 0x160},
             {"scaron", 0x161},
             {"Yuml", 0x178},
             {"fnof", 0x192},
             {"circ", 0x2C6},
             {"tilde", 0x2DC},
             {"DiacriticalTilde", 0x2DC},
             /* Letterlike symbols */
             {"trade", 0x2122},
             {"TRADE", 0x2122},
             {"weierp", 0x2118},
             {"wp", 0x2118},
             {"image", 0x2111},
             {"Im", 0x2111},
             {"real", 0x211C},
             {"Re", 0x211C},
             {"alefsym", 0x2135},
             {"aleph", 0x2135},
             {"ell", 0x2113},
             {"naturals", 0x2115},
             {"Nopf", 0x2115},
             {"Ropf", 0x211D},
             {"Zopf", 0x2124},
             {"Qopf", 0x211A},
             {"numero", 0x2116},
             {"copysr", 0x2117},
             {"rx", 0x211E},
             /* Arrows */
             {"larr", 0x2190},
             {"leftarrow", 0x2190},
             {"uarr", 0x2191},
             {"uparrow", 0x2191},
             {"rarr", 0x2192},
             {"rightarrow", 0x2192},
             {"darr", 0x2193},
             {"downarrow", 0x2193},
             {"harr", 0x2194},
             {"leftrightarrow", 0x2194},
             {"varr", 0x2195},
             {"crarr", 0x21B5},
             {"lArr", 0x21D0},
             {"uArr", 0x21D1},
             {"rArr", 0x21D2},
             {"Rightarrow", 0x21D2},
             {"dArr", 0x21D3},
             {"hArr", 0x21D4},
             {"Leftrightarrow", 0x21D4},
             /* Mathematical operators */
             {"forall", 0x2200},
             {"part", 0x2202},
             {"exist", 0x2203},
             {"empty", 0x2205},
             {"emptyset", 0x2205},
             {"nabla", 0x2207},
             {"isin", 0x2208},
             {"in", 0x2208},
             {"notin", 0x2209},
             {"ni", 0x220B},
             {"prod", 0x220F},
             {"sum", 0x2211},
             {"minus", 0x2212},
             {"lowast", 0x2217},
             {"radic", 0x221A},
             {"Sqrt", 0x221A},
             {"prop", 0x221D},
             {"propto", 0x221D},
             {"infin", 0x221E},
             {"ang", 0x2220},
             {"angle", 0x2220},
             {"and", 0x2227},
             {"wedge", 0x2227},
             {"or", 0x2228},
             {"vee", 0x2228},
             {"cap", 0x2229},
             {"cup", 0x222A},
             {"int", 0x222B},
             {"Integral", 0x222B},
             {"there4", 0x2234},
             {"therefore", 0x2234},
             {"becaus", 0x2235},
             {"sim", 0x223C},
             {"cong", 0x2245},
             {"asymp", 0x2248},
             {"ne", 0x2260},
             {"equiv", 0x2261},
             {"le", 0x2264},
             {"leq", 0x2264},
             {"ge", 0x2265},
             {"geq", 0x2265},
             {"sub", 0x2282},
             {"subset", 0x2282},
             {"sup", 0x2283},
             {"supset", 0x2283},
             {"nsub", 0x2284},
             {"sube", 0x2286},
             {"subseteq", 0x2286},
             {"supe", 0x2287},
             {"supseteq", 0x2287},
             {"oplus", 0x2295},
             {"otimes", 0x2297},
             {"perp", 0x22A5},
             {"sdot", 0x22C5},
             /* Technical / misc symbols */
             {"lceil", 0x2308},
             {"rceil", 0x2309},
             {"lfloor", 0x230A},
             {"rfloor", 0x230B},
             {"lang", 0x27E8},
             {"langle", 0x27E8},
             {"rang", 0x27E9},
             {"rangle", 0x27E9},
             {"loz", 0x25CA},
             {"lozenge", 0x25CA},
             {"spades", 0x2660},
             {"spadesuit", 0x2660},
             {"clubs", 0x2663},
             {"clubsuit", 0x2663},
             {"hearts", 0x2665},
             {"heartsuit", 0x2665},
             {"diams", 0x2666},
             {"diamondsuit", 0x2666},
             {"star", 0x2606},
             {"starf", 0x2605},
             {"bigstar", 0x2605},
             {"phone", 0x260E},
             {"female", 0x2640},
             {"male", 0x2642},
             {"check", 0x2713},
             {"checkmark", 0x2713},
             {"cross", 0x2717},
             {"sung", 0x266A},
             {"flat", 0x266D},
             {"natur", 0x266E},
             {"sharp", 0x266F},
             /* Currency */
             {"euro", 0x20AC},
             /* Greek lowercase */
             {"alpha", 0x3B1},
             {"beta", 0x3B2},
             {"gamma", 0x3B3},
             {"delta", 0x3B4},
             {"epsilon", 0x3B5},
             {"epsiv", 0x3B5},
             {"zeta", 0x3B6},
             {"eta", 0x3B7},
             {"theta", 0x3B8},
             {"iota", 0x3B9},
             {"kappa", 0x3BA},
             {"lambda", 0x3BB},
             {"mu", 0x3BC},
             {"nu", 0x3BD},
             {"xi", 0x3BE},
             {"omicron", 0x3BF},
             {"pi", 0x3C0},
             {"rho", 0x3C1},
             {"sigmaf", 0x3C2},
             {"sigmav", 0x3C2},
             {"sigma", 0x3C3},
             {"tau", 0x3C4},
             {"upsilon", 0x3C5},
             {"upsi", 0x3C5},
             {"phi", 0x3C6},
             {"chi", 0x3C7},
             {"psi", 0x3C8},
             {"omega", 0x3C9},
             {"thetasym", 0x3D1},
             {"thetav", 0x3D1},
             {"upsih", 0x3D2},
             {"piv", 0x3D6},
             /* Greek uppercase */
             {"Alpha", 0x391},
             {"Beta", 0x392},
             {"Gamma", 0x393},
             {"Delta", 0x394},
             {"Epsilon", 0x395},
             {"Zeta", 0x396},
             {"Eta", 0x397},
             {"Theta", 0x398},
             {"Iota", 0x399},
             {"Kappa", 0x39A},
             {"Lambda", 0x39B},
             {"Mu", 0x39C},
             {"Nu", 0x39D},
             {"Xi", 0x39E},
             {"Omicron", 0x39F},
             {"Pi", 0x3A0},
             {"Rho", 0x3A1},
             {"Sigma", 0x3A3},
             {"Tau", 0x3A4},
             {"Upsilon", 0x3A5},
             {"Phi", 0x3A6},
             {"Chi", 0x3A7},
             {"Psi", 0x3A8},
             {"Omega", 0x3A9},
             {0, 0}};
  /* Bucket the table by first letter on first use, so each &name */
  static short ent_head[256];                        /* first char -> entry index + 1 (0 = empty) */
  static short ent_next[sizeof ent / sizeof ent[0]]; /* chain within a bucket */
  static int ent_built;
  if (!ent_built) {
    for (int i = 0; ent[i].name; i++) {
      unsigned char fc = (unsigned char)ent[i].name[0];
      ent_next[i] = ent_head[fc];
      ent_head[fc] = (short)(i + 1);
    }
    ent_built = 1;
  }
  for (int e = ent_head[(unsigned char)body[0]]; e; e = ent_next[e - 1]) {
    const char *nm = ent[e - 1].name;
    int k = 0;
    while (nm[k] && k < blen && nm[k] == body[k])
      k++;
    if (!nm[k] && k == blen) {
      *adv = n + 1;
      return ent[e - 1].ch;
    }
  }
  *adv = 1;
  return '&';
}

/* Decode HTML entities in a string in place - mainly so href/src attribute values (e.g */
void decode_entities(char *s)
{
  char *o = s;
  const char *p = s, *end = s + strlen(s);
  while (p < end) {
    if (*p == '&') {
      int adv, c = decode_entity(p, end, &adv);
      p += adv;
      if (c == -2)
        continue; /* drop                            */
      if (c == -1) {
        *o++ = ' ';
        continue;
      }
      o += utf8_encode((unsigned)c, o);
    } else {
      *o++ = *p++;
    }
  }
  *o = '\0';
}

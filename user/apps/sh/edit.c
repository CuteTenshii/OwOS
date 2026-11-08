/* edit.c: the prompt and the line editor */

#include "sh.h"

void prompt(void)
{
  tputs_c(username, FG_PROMPT);
  tputs_c("@", FG_PROMPT);
  tputs_c(hostnm, FG_PROMPT);
  tputs_c(":", FG_PROMPT);
  int hl = strlen(home);
  if (strcmp(cwd, home) == 0)
    tputs_c("~", FG_PROMPT);
  else if (strlen(cwd) > hl && cwd[hl] == '/' && has_prefix(cwd, home)) {
    tputs_c("~", FG_PROMPT);
    tputs_c(cwd + hl, FG_PROMPT);
  } else
    tputs_c(cwd, FG_PROMPT);
  /* '#' for root, '$' for everyone else, the classic privilege cue */
  tputs_c(strcmp(username, "root") == 0 ? "# " : "$ ", FG_PROMPT);
}

/* line editor */

char *line;

static int llen, lpos, lcap;

static char **hist;

static int hcount, hpos;

static int getb(void)
{
  unsigned char c;
  return read(0, &c, 1) == 1 ? c : -1;
}

static void line_insert(char c)
{
  if (llen + 1 >= lcap) {
    char *grown = realloc(line, (unsigned)(lcap ? lcap * 2 : 128));
    if (!grown)
      return;
    line = grown;
    lcap = lcap ? lcap * 2 : 128;
  }
  for (int i = llen; i > lpos; i--)
    line[i] = line[i - 1];
  line[lpos++] = c;
  line[++llen] = '\0';
}

static void line_delete(int from, int to)
{
  for (int i = to; i <= llen; i++)
    line[from + i - to] = line[i];
  llen -= to - from;
  if (lpos > from)
    lpos = lpos > to ? lpos - (to - from) : from;
}

static void line_set(const char *s)
{
  llen = lpos = 0;
  if (line)
    line[0] = '\0';
  for (; *s; s++)
    line_insert(*s);
}

/* Redraw prompt and line, then put the caret back; assumes one terminal row */
static void line_show(void)
{
  tputc('\r');
  prompt();
  for (int i = 0; i < llen; i++)
    tputc(line[i]);
  tputs("\x1b[K");
  if (lpos < llen) {
    tputs("\x1b[");
    tdec((unsigned)(llen - lpos));
    tputc('D');
  }
  oflush();
}

void history_add(void)
{
  if (!llen || (hcount && streq(hist[hcount - 1], line)))
    return;
  char **grown = realloc(hist, (unsigned)(hcount + 1) * sizeof *hist);
  char *copy = malloc((unsigned)llen + 1);
  if (!grown || !copy) {
    free(copy);
    return;
  }
  strcpy(copy, line);
  hist = grown;
  hist[hcount++] = copy;
}

static void history_step(int dir)
{
  int next = hpos + dir;
  if (next < -1 || next >= hcount)
    return;
  hpos = next;
  line_set(hpos < 0 ? "" : hist[hcount - 1 - hpos]);
}

static void escape_key(void)
{
  int c = getb();
  if (c != '[' && c != 'O')
    return;
  int param = 0;
  while ((c = getb()) >= 0 && c >= '0' && c <= ';')
    param = c == ';' ? 0 : param * 10 + (c - '0');
  switch (c) {
  case 'A':
    history_step(1);
    break;
  case 'B':
    history_step(-1);
    break;
  case 'C':
    if (lpos < llen)
      lpos++;
    break;
  case 'D':
    if (lpos > 0)
      lpos--;
    break;
  case 'H':
    lpos = 0;
    break;
  case 'F':
    lpos = llen;
    break;
  case '~':
    if (param == 3 && lpos < llen)
      line_delete(lpos, lpos + 1);
    else if (param == 1 || param == 7)
      lpos = 0;
    else if (param == 4 || param == 8)
      lpos = llen;
    break;
  }
}

/* Read one edited line into `line`; false at end of input */
bool edit_line(void)
{
  llen = lpos = 0;
  hpos = -1;
  if (line)
    line[0] = '\0';
  for (;;) {
    oflush();
    int c = getb();
    if (c < 0)
      return false;
    switch (c) {
    case '\r':
    case '\n':
      tputc('\n');
      oflush();
      return true;
    case 0x03: /* Ctrl+C: abandon the line */
      tputs("^C\n");
      llen = lpos = 0;
      if (line)
        line[0] = '\0';
      break;
    case 0x04: /* Ctrl+D: end of input on an empty line, else delete */
      if (!llen) {
        tputc('\n');
        return false;
      }
      if (lpos < llen)
        line_delete(lpos, lpos + 1);
      break;
    case 0x7f:
    case '\b':
      if (lpos > 0)
        line_delete(lpos - 1, lpos);
      break;
    case 0x01:
      lpos = 0;
      break;
    case 0x05:
      lpos = llen;
      break;
    case 0x0b: /* kill to end of line */
      line_delete(lpos, llen);
      break;
    case 0x15: /* kill the whole line */
      line_delete(0, llen);
      break;
    case 0x17: { /* kill the word before the caret */
      int from = lpos;
      while (from > 0 && line[from - 1] == ' ')
        from--;
      while (from > 0 && line[from - 1] != ' ')
        from--;
      line_delete(from, lpos);
      break;
    }
    case 0x0c: /* Ctrl+L: clear the screen */
      tputs("\x1b[2J\x1b[H");
      break;
    case 0x1b:
      escape_key();
      break;
    default:
      if (c >= 0x20 && c < 0x7f)
        line_insert((char)c);
      break;
    }
    line_show();
  }
}

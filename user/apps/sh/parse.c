/* parse.c: splitting a command line into words, quoting and $ expansion */

#include "sh.h"

static char *strdup_empty(void)
{
  char *e = malloc(1);
  if (e)
    *e = '\0';
  return e;
}

/* Append to a growing string */
struct word {
  char *s;
  unsigned len, cap;
};

static bool word_put(struct word *w, const char *p, unsigned n)
{
  if (w->len + n + 1 > w->cap) {
    unsigned cap = (w->len + n + 1) * 2;
    char *grown = realloc(w->s, cap);
    if (!grown)
      return false;
    w->s = grown;
    w->cap = cap;
  }
  memcpy(w->s + w->len, p, n);
  w->len += n;
  w->s[w->len] = '\0';
  return true;
}

/* Expand the variable named at text[*i] (just past a '$') into w */
static bool expand_var(struct word *w, const char *text, int *i)
{
  const char *name = text + *i;
  int n = 0;
  if (*name == '?' || *name == '!') {
    char num[12];
    int digits = 0;
    unsigned v = (unsigned)(*name == '?' ? last_status : last_bg_pid);
    do {
      num[digits++] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    for (int k = 0; k < digits; k++)
      if (!word_put(w, &num[digits - 1 - k], 1))
        return false;
    (*i)++;
    return true;
  }
  bool braced = *name == '{';
  if (braced)
    name++;
  while (name[n] && ((name[n] >= 'a' && name[n] <= 'z') || (name[n] >= 'A' && name[n] <= 'Z') ||
                     (name[n] >= '0' && name[n] <= '9') || name[n] == '_'))
    n++;
  if (!valid_var_name(name, n) || (braced && name[n] != '}'))
    return word_put(w, "$", 1); /* not a variable: keep the '$' */
  char key[128];
  if (n >= (int)sizeof key)
    return true;
  memcpy(key, name, (unsigned)n);
  key[n] = '\0';
  *i += (int)(name - (text + *i)) + n + (braced ? 1 : 0);
  const char *value = getenv(key);
  return !value || word_put(w, value, (unsigned)strlen(value));
}

/* Split a command line into words (whitespace-separated; '', "" and \ quote; $NAME, ${NAME} and $? expand
 * outside single quotes) and run it. A line of NAME=value words only sets environment variables. */
void run_line(const char *text)
{
  unsigned len = (unsigned)strlen(text);
  char **argv = malloc((len / 2 + 2) * sizeof *argv);
  int argc = 0;
  if (!argv)
    return;
  struct word w = {0, 0, 0};
  bool in_word = false, ok = true, background = false;
  char quote = 0;
  current_line = text;
  for (int i = 0; text[i] && ok;) {
    char c = text[i++];
    if (c == '#' && !quote && !in_word) /* a comment runs to the end of the line */
      break;
    if (c == '&' && !quote) { /* a trailing & runs the command in the background */
      int k = i;
      while (text[k] == ' ' || text[k] == '\t')
        k++;
      background = !text[k];
      if (background) {
        if (in_word) {
          argv[argc++] = w.s ? w.s : strdup_empty();
          w.s = 0;
          w.len = w.cap = 0;
          in_word = false;
        }
        break;
      }
    }
    if (quote == '\'') {
      if (c == '\'')
        quote = 0;
      else
        ok = word_put(&w, &c, 1);
    } else if (c == '\\' && text[i]) {
      in_word = true;
      ok = word_put(&w, &text[i++], 1);
    } else if (c == '\'' || c == '"') {
      in_word = true;
      if (!quote)
        quote = c;
      else if (quote == c)
        quote = 0;
      else
        ok = word_put(&w, &c, 1);
    } else if (c == '$') {
      in_word = true;
      ok = expand_var(&w, text, &i);
    } else if ((c == ' ' || c == '\t') && !quote) {
      if (in_word) {
        argv[argc++] = w.s ? w.s : strdup_empty();
        w.s = 0;
        w.len = w.cap = 0;
        in_word = false;
      }
    } else {
      in_word = true;
      ok = word_put(&w, &c, 1);
    }
  }
  if (in_word && ok)
    argv[argc++] = w.s ? w.s : strdup_empty();
  else
    free(w.s);
  argv[argc] = 0;
  bool assignments = argc > 0;
  for (int i = 0; i < argc; i++)
    assignments = assignments && is_assignment(argv[i]);
  if (assignments) {
    for (int i = 0; i < argc; i++)
      set_assignment(argv[i]);
  } else if (argc && ok && background) {
    if (is_builtin(argv[0]))
      err(argv[0], ": a builtin cannot run in the background");
    else
      run_other(argc, argv, true);
  } else if (argc && ok) {
    run_argv(argc, argv);
  }
  for (int i = 0; i < argc; i++)
    free(argv[i]);
  free(argv);
}

/* Run the commands in a file, one per line; false when it cannot be read */
bool run_file(const char *path)
{
  struct statinfo st;
  if (stat(path, &st) != 0 || st.is_dir)
    return false;
  char *text = malloc(st.size + 1);
  if (!text)
    return false;
  int n = readfile(path, text, (int)st.size);
  if (n < 0) {
    free(text);
    return false;
  }
  text[n] = '\0';
  for (char *line = text; *line;) {
    char *end = line;
    while (*end && *end != '\n')
      end++;
    char next = *end;
    *end = '\0';
    if (end > line && end[-1] == '\r')
      end[-1] = '\0';
    run_line(line);
    line = next ? end + 1 : end;
  }
  free(text);
  return true;
}

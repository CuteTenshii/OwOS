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

/* A parsed command line */
struct command {
  char **argv;
  int argc;
  char *target; /* the file `>` or `>>` sends the output to */
  bool append, background;
};

static void command_free(struct command *c)
{
  for (int i = 0; i < c->argc; i++)
    free(c->argv[i]);
  free(c->argv);
  free(c->target);
}

/* Finish the word in w: it names the redirect target when one is pending, else it is the next argument */
static void end_word(struct word *w, struct command *c, bool *want_target)
{
  char *s = w->s ? w->s : strdup_empty();
  if (*want_target) {
    free(c->target);
    c->target = s;
    *want_target = false;
  } else {
    c->argv[c->argc++] = s;
  }
  w->s = 0;
  w->len = w->cap = 0;
}

/* Split a command line into words (whitespace-separated; '', "" and \ quote; $NAME, ${NAME} and $? expand
 * outside single quotes), a trailing & and a `> file` or `>> file`. False, after reporting, when it is malformed. */
static bool parse_command(const char *text, struct command *c)
{
  *c = (struct command){0};
  c->argv = malloc((strlen(text) / 2 + 2) * sizeof *c->argv);
  if (!c->argv)
    return false;
  struct word w = {0, 0, 0};
  bool in_word = false, ok = true, want_target = false;
  char quote = 0;
  for (int i = 0; text[i] && ok;) {
    char ch = text[i++];
    if (ch == '#' && !quote && !in_word) /* a comment runs to the end of the line */
      break;
    if (ch == '&' && !quote) {
      int k = i;
      while (text[k] == ' ' || text[k] == '\t')
        k++;
      c->background = !text[k];
      if (c->background) {
        if (in_word) {
          end_word(&w, c, &want_target);
          in_word = false;
        }
        break;
      }
    }
    if (quote == '\'') {
      if (ch == '\'')
        quote = 0;
      else
        ok = word_put(&w, &ch, 1);
    } else if (ch == '\\' && text[i]) {
      in_word = true;
      ok = word_put(&w, &text[i++], 1);
    } else if (ch == '\'' || ch == '"') {
      in_word = true;
      if (!quote)
        quote = ch;
      else if (quote == ch)
        quote = 0;
      else
        ok = word_put(&w, &ch, 1);
    } else if (ch == '$') {
      in_word = true;
      ok = expand_var(&w, text, &i);
    } else if ((ch == ' ' || ch == '\t') && !quote) {
      if (in_word) {
        end_word(&w, c, &want_target);
        in_word = false;
      }
    } else if (ch == '>' && !quote) {
      if (in_word) {
        end_word(&w, c, &want_target);
        in_word = false;
      }
      if (want_target) {
        err("sh: syntax error near '>'", 0);
        ok = false;
      }
      c->append = text[i] == '>';
      i += c->append;
      want_target = true;
    } else {
      in_word = true;
      ok = word_put(&w, &ch, 1);
    }
  }
  if (in_word && ok)
    end_word(&w, c, &want_target);
  else
    free(w.s);
  c->argv[c->argc] = 0;
  if (ok && want_target) {
    err("sh: syntax error: missing file after '>'", 0);
    ok = false;
  }
  if (ok && c->target && (c->background || !c->argc)) {
    err(c->background ? "sh: cannot redirect a background job" : "sh: missing command before '>'", 0);
    ok = false;
  }
  return ok;
}

/* Run a parsed command. A line of NAME=value words only sets environment variables. */
static void run_command(const struct command *c)
{
  bool assignments = true;
  for (int i = 0; i < c->argc; i++)
    assignments = assignments && is_assignment(c->argv[i]);
  if (assignments) {
    for (int i = 0; i < c->argc; i++)
      set_assignment(c->argv[i]);
  } else if (c->background) {
    if (is_builtin(c->argv[0]))
      err(c->argv[0], ": a builtin cannot run in the background");
    else
      run_other(c->argc, c->argv, true);
  } else if (c->target) {
    run_redirected(c->argc, c->argv, c->target, c->append);
  } else {
    run_argv(c->argc, c->argv);
  }
}

void run_line(const char *text)
{
  struct command c;
  current_line = text;
  if (parse_command(text, &c) && c.argc)
    run_command(&c);
  command_free(&c);
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

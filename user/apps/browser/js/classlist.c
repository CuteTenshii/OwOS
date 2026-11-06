#include <stdbool.h>

#include "classlist.h"
#include "owos.h"

static char *dup_empty(void)
{
  char *s = malloc(1);
  if (s)
    s[0] = '\0';
  return s;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

int classlist_has(const char *classes, const char *token, int length)
{
  if (!classes || !token || length <= 0)
    return false;
  while (*classes) {
    const char *start;
    while (is_space(*classes))
      classes++;
    start = classes;
    while (*classes && !is_space(*classes))
      classes++;
    if (classes - start == length && !memcmp(start, token, (unsigned)length))
      return true;
  }
  return false;
}

int classlist_count(const char *classes)
{
  int count = 0;
  if (!classes)
    return 0;
  while (*classes) {
    while (is_space(*classes))
      classes++;
    if (!*classes)
      break;
    count++;
    while (*classes && !is_space(*classes))
      classes++;
  }
  return count;
}

const char *classlist_nth(const char *classes, int index, int *length)
{
  int count = 0;
  if (!classes || index < 0)
    return 0;
  while (*classes) {
    const char *start;
    while (is_space(*classes))
      classes++;
    if (!*classes)
      break;
    start = classes;
    while (*classes && !is_space(*classes))
      classes++;
    if (count++ == index) {
      *length = (int)(classes - start);
      return start;
    }
  }
  return 0;
}

static bool append(char **out, int *length, int *capacity, const char *token, int token_length)
{
  int need;
  if (token_length <= 0)
    return true;
  need = *length + token_length + (*length ? 1 : 0) + 1;
  if (need > *capacity) {
    int next = *capacity ? *capacity * 2 : 32;
    while (next < need)
      next *= 2;
    {
      char *grown = realloc(*out, (unsigned)next);
      if (!grown)
        return false;
      *out = grown;
    }
    *capacity = next;
  }
  if (*length)
    (*out)[(*length)++] = ' ';
  memcpy(*out + *length, token, (unsigned)token_length);
  *length += token_length;
  (*out)[*length] = 0;
  return true;
}

static void rebuild(char **classes, const char *replace, int replace_length, const char *remove, int remove_length)
{
  const char *token;
  char *out = 0;
  int out_length = 0, capacity = 0, index, length;
  const char *current = *classes ? *classes : "";

  for (index = 0; (token = classlist_nth(current, index, &length)); index++) {
    if (replace && length == remove_length && !memcmp(token, remove, (unsigned)length))
      token = replace, length = replace_length;
    else if (remove && length == remove_length && !memcmp(token, remove, (unsigned)length))
      continue;
    if (!classlist_has(out, token, length))
      if (!append(&out, &out_length, &capacity, token, length)) {
        free(out);
        return;
      }
  }
  free(*classes);
  *classes = out ? out : dup_empty();
}

void classlist_add(char **classes, const char *token, int length)
{
  char *out = 0;
  int out_length = 0, capacity = 0, index, token_length;
  const char *current = *classes ? *classes : "";
  const char *item;
  for (index = 0; (item = classlist_nth(current, index, &token_length)); index++)
    if (!classlist_has(out, item, token_length))
      if (!append(&out, &out_length, &capacity, item, token_length)) {
        free(out);
        return;
      }
  if (token && length > 0 && !classlist_has(out, token, length))
    if (!append(&out, &out_length, &capacity, token, length)) {
      free(out);
      return;
    }
  free(*classes);
  *classes = out ? out : dup_empty();
}

void classlist_remove(char **classes, const char *token, int length) { rebuild(classes, 0, 0, token, length); }

int classlist_replace(char **classes, const char *old_token, const char *new_token)
{
  int old_length = old_token ? (int)strlen(old_token) : 0;
  int new_length = new_token ? (int)strlen(new_token) : 0;
  if (!old_length || !new_length || !classlist_has(*classes, old_token, old_length))
    return 0;
  rebuild(classes, new_token, new_length, old_token, old_length);
  return 1;
}

int classlist_toggle(char **classes, const char *token)
{
  int length = token ? (int)strlen(token) : 0;
  if (!length)
    return 0;
  if (classlist_has(*classes, token, length)) {
    classlist_remove(classes, token, length);
    return 0;
  }
  classlist_add(classes, token, length);
  return 1;
}

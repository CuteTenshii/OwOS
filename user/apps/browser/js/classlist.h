#ifndef BROWSER_CLASSLIST_H
#define BROWSER_CLASSLIST_H

int classlist_has(const char *classes, const char *token, int length);
int classlist_count(const char *classes);
const char *classlist_nth(const char *classes, int index, int *length);
void classlist_add(char **classes, const char *token, int length);
void classlist_remove(char **classes, const char *token, int length);
int classlist_replace(char **classes, const char *old_token, const char *new_token);
int classlist_toggle(char **classes, const char *token);

#endif

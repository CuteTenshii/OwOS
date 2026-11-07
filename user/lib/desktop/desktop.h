/* desktop.h: freedesktop Desktop Entry parser (subset), Exec expansion and directory scanner.
 * Bare Exec names are looked up in /bin by the launcher. OnlyShowIn id: "OwOS". */
#ifndef OWOS_DESKTOP_H
#define OWOS_DESKTOP_H

#include <stdbool.h>

#define DESKTOP_ENVIRONMENT "OwOS"
#define DESKTOP_ORDER_DEFAULT 1000000

struct desktop_strv {
  char **v;
  int n;
};

struct desktop_action {
  char *id; /* "new-window" for [Desktop Action new-window] */
  char *name;
  char *exec; /* may be NULL */
  char *icon;
};

struct desktop_entry {
  char *id;       /* file name without .desktop */
  char *location; /* path it was read from (%k), NULL when parsed from memory */
  char *name, *generic_name, *comment, *exec, *try_exec, *path, *icon, *wm_class;
  bool terminal, no_display, hidden;
  int order; /* X-OwOS-Order, lower first */
  struct desktop_strv categories, keywords, mime_types, only_show_in, not_show_in;
  struct desktop_action *actions; /* only those listed in Actions= */
  int nactions;
  char *type;
  struct desktop_strv action_ids;
};

struct desktop_list {
  struct desktop_entry **v;
  int n;
};

/* NULL on OOM, missing Name/Exec, Type != Application or Hidden=true. */
struct desktop_entry *desktop_parse(const char *text, int len, const char *id);
void desktop_free(struct desktop_entry *e);

/* Expand Exec (or action's Exec when action != NULL) into a NULL-terminated argv. 0 or -1. */
int desktop_exec_argv(const struct desktop_entry *e, const struct desktop_action *action, const char *const *files,
                      int nfiles, char ***argv, int *argc);
void desktop_argv_free(char **argv, int argc);

/* Menu visibility: NoDisplay, Hidden, OnlyShowIn/NotShowIn and TryExec existence. */
bool desktop_visible(const struct desktop_entry *e);

/* Parse every *.desktop in dir into out (same id replaces the earlier one); count, or -1 on OOM. */
int desktop_scan(const char *dir, struct desktop_list *out);
void desktop_sort(struct desktop_list *l);
void desktop_list_free(struct desktop_list *l);

#endif

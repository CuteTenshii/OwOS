/* devhub.h, shared state for the Device Hub app */
#ifndef DEVHUB_H
#define DEVHUB_H

#include "parse.h"
#include <stdbool.h>

#define MAX_ROWS 192

enum row_kind {
  ROW_FIELD, /* label + value */
  ROW_HEAD,  /* a group heading */
  ROW_NOTE,  /* a full-width dim line */
  ROW_ERROR, /* a read that failed */
};

struct row {
  unsigned char kind;
  unsigned char indent; /* hub depth for nested USB devices */
  char label[40];
  char value[120];
};

struct page {
  struct row rows[MAX_ROWS];
  int n;
};

struct category {
  const char *name;
  bool (*present)(void);
  void (*load)(struct page *pg);
  bool live;   /* re-read on the refresh timer */
  bool costly; /* reading it probes hardware: show a loading frame, never poll */
};

extern const struct category categories[];
extern const int ncategories;

#endif /* DEVHUB_H */

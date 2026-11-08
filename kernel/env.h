/* env.h - a process environment: consecutive "NAME=VALUE" strings, each NUL-terminated. */
#ifndef OWOS_ENV_H
#define OWOS_ENV_H

#include <stdbool.h>
#include <stdint.h>

struct env {
  char *data; /* NULL while empty */
  uint32_t len;
};

const char *env_get(const struct env *e, const char *name); /* the value, or NULL */
int env_set(struct env *e, const char *name, const char *value,
            bool overwrite); /* 0, or -1 on a bad name / no memory */
void env_unset(struct env *e, const char *name);
const char *env_entry(const struct env *e, uint32_t index);         /* "NAME=VALUE" or NULL past the end */
int env_copy(struct env *dst, const struct env *src);               /* dst must be empty */
int env_from_block(struct env *e, const char *block, uint32_t len); /* e must be empty; entries need a '=' */
void env_free(struct env *e);

#endif

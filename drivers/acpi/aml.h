/* aml.h: tiny best-effort ACPI AML interpreter for reading battery charge via _BST/_BIF. */
#ifndef OWOS_AML_H
#define OWOS_AML_H

#include <stdbool.h>

struct aml_battery {
  int percent;      /* 0..100 remaining charge                              */
  bool charging;    /* 1 if the _BST state says charging                    */
  bool discharging; /* 1 if discharging                                     */
  int minutes;      /* est. minutes to empty when discharging, else -1      */
};

/* Evaluate the first battery's _BST/_BIF into *out. Returns 1 on success, else 0. */
bool aml_read_battery(struct aml_battery *out);

/* Returns 0 only if _STA positively reports the battery absent; 1 if present or unknown. */
bool aml_battery_present(void);

/* Note on why the last aml_read_battery() returned 0; empty after success. */
const char *aml_last_error(void);

/* Namespace-walk diagnostics: object count and first bail opcode (-1 if none). NULL allowed. */
void aml_ns_stats(int *out_nodes, int *out_bail);

/* Verbose battery dump (raw _BST/_BIF fields, structure counts) into out, NUL-terminated. */
void aml_battery_debug(char *out, int max);

#endif /* OWOS_AML_H */

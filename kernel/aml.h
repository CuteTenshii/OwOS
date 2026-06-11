/* aml.h — a tiny, best-effort ACPI AML interpreter, scoped to one job:
 * reading the battery charge by evaluating the Control Method Battery's _BST
 * and _BIF/_BIX methods. It is NOT a general ACPICA replacement — it implements
 * the opcode subset those methods use in practice (field reads off the Embedded
 * Controller, Store/Index, If/While, arithmetic, method calls, Packages) and
 * fails gracefully (returns 0) on anything it doesn't understand, so the worst
 * case is "no percentage", never a crash or a hang. */
#ifndef OWOS_AML_H
#define OWOS_AML_H

struct aml_battery {
	int percent;        /* 0..100 remaining charge                              */
	int charging;       /* 1 if the _BST state says charging                    */
	int discharging;    /* 1 if discharging                                     */
};

/* Evaluate the first ACPI battery's _BST/_BIF and fill *out. Returns 1 on
 * success, 0 if there's no battery, ACPI is unreachable, or a method couldn't
 * be evaluated. Safe to call repeatedly; the namespace is parsed once. */
int aml_read_battery(struct aml_battery *out);

/* A short human-readable note on why the last aml_read_battery() returned 0
 * (e.g. "unsupported opcode 0xA4", "_BST not found"). For diagnostics on real
 * hardware where this can't be tested locally. Empty string after success. */
const char *aml_last_error(void);

#endif /* OWOS_AML_H */

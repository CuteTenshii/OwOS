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
	int minutes;        /* est. minutes to empty when discharging, else -1      */
};

/* Evaluate the first ACPI battery's _BST/_BIF and fill *out. Returns 1 on
 * success, 0 if there's no battery, ACPI is unreachable, or a method couldn't
 * be evaluated. Safe to call repeatedly; the namespace is parsed once. */
int aml_read_battery(struct aml_battery *out);

/* Returns 0 only if the battery device's _STA positively reports the battery
 * absent (bit 4 clear) — e.g. VMware's phantom battery. Returns 1 when _STA is
 * missing or can't be evaluated (assume present; never suppress a real battery
 * we just can't query). The caller still byte-scans for the _HID first. */
int aml_battery_present(void);

/* A short human-readable note on why the last aml_read_battery() returned 0
 * (e.g. "unsupported opcode 0xA4", "_BST not found"). For diagnostics on real
 * hardware where this can't be tested locally. Empty string after success. */
const char *aml_last_error(void);

/* Namespace-walk diagnostics (real-hardware battery debugging). *out_nodes gets
 * the count of recorded ACPI objects; *out_bail gets the opcode at which a
 * term-list walk first bailed (0x5B00|ext for an ExtOp, else the bare opcode),
 * or -1 if none. Either argument may be NULL. */
void aml_ns_stats(int *out_nodes, int *out_bail);

/* Verbose battery dump for real-hardware debugging: the raw _BST/_BIF field
 * values plus structural counts (_REG methods, EmbeddedControl regions, EC
 * devices). Writes a bounded, NUL-terminated one-liner into out. */
void aml_battery_debug(char *out, int max);

#endif /* OWOS_AML_H */

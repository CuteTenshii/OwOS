/* power.c: the shared power.h API over PSCI. There is no suspend-to-RAM path on these boards yet. */
#include "power.h"
#include "arch.h"

void power_off(void) { arch_poweroff(); }

void power_reset(void) { arch_reset(); }

bool power_can_sleep(void) { return false; }

/* Only reached if a caller ignores power_can_sleep(): there is nothing to suspend into, so it returns at once. */
void power_sleep(void) {}

/* nic.c — NIC driver dispatch.
 *
 * Holds the table of link-layer drivers and the active one. nic_init() probes
 * them in priority order (Intel gigabit first, the RTL8139 QEMU default last)
 * and keeps the first whose device is present; nic_present/mac/send then route
 * to it. This is the whole multi-driver story: net.c calls these four symbols
 * and never names a chip.
 */

#include <stddef.h>

#include "console.h"
#include "nic.h"

extern const struct nic_ops igb_ops;
extern const struct nic_ops e1000_ops;
extern const struct nic_ops rtl8139_ops;

/* Probe order. The Intel gigabit drivers come first so a real machine's onboard
 * Intel part is preferred; rtl8139 is the fallback QEMU attaches by default.
 * igb (I210/I211/82576) and e1000 (8254x/82574/I217-I219) match disjoint
 * device-ID sets, so their relative order is cosmetic — igb leads as the newer
 * family. */
static const struct nic_ops *const drivers[] = {
    &igb_ops,
    &e1000_ops,
    &rtl8139_ops,
};

static const struct nic_ops *active;

void nic_init(void)
{
  for (unsigned i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
    if (drivers[i]->probe()) {
      active = drivers[i];
      return;
    }
  }
  con_write("net: no NIC found\n");
}

/* Install a driver discovered after boot (the USB-tethering RNDIS NIC, which
 * only appears once a phone is plugged in and tethering enabled). Overrides any
 * driver the boot-time probe found. */
void nic_set(const struct nic_ops *ops) { active = ops; }

int nic_present(void) { return active != NULL; }

const uint8_t *nic_mac(void) { return active ? active->mac() : NULL; }

const char *nic_name(void) { return active ? active->name : NULL; }

void nic_send(const uint8_t *frame, uint32_t len)
{
  if (active)
    active->send(frame, len);
}

void nic_poll(void)
{
  if (active && active->poll)
    active->poll();
}

int nic_link_up(void)
{
  if (!active)
    return 0;
  return active->link_up ? active->link_up() : 1;
}

/* virtioserial.h: virtio-serial (console) transport for the QEMU host agents.
 *
 * Modern virtio-pci, polled by one kernel thread. Each client owns a named port (QEMU `-device
 * virtserialport,...,name=<name>,nr=<port>`); the thread feeds received bytes to client->rx and calls
 * client->tick (if set) every poll. */
#ifndef OWOS_VIRTIOSERIAL_H
#define OWOS_VIRTIOSERIAL_H

#include <stdbool.h>
#include <stdint.h>

struct vserial_client {
  uint32_t port; /* port id, >= 1 (queues 2*port+2 / 2*port+3) */
  const char *name;
  void (*rx)(const uint8_t *data, uint32_t len);
  void (*tick)(void);
};

extern const struct vserial_client vdagent_client, qemuga_client;

/* Find the device, bring the ports up and start the poll thread. No-op and silent if absent. Needs the scheduler. */
void vserial_init(void);

/* True once the host announced the port (the QEMU side exists). */
bool vserial_port_present(uint32_t port);

/* True while both ends have the port open, so writes reach the host. */
bool vserial_port_open(uint32_t port);

/* Send len <= 4096 bytes. Returns false if the port is closed or the device didn't take them. */
bool vserial_write(uint32_t port, const void *data, uint32_t len);

#endif /* OWOS_VIRTIOSERIAL_H */

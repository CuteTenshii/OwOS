/* vmxnet3.h — VMware vmxnet3 paravirtual Ethernet driver (PCI 15AD:07B0).
 *
 * Exports a nic_ops the dispatch layer (nic.c) probes. Single TX/RX queue,
 * device revision 1, no offloads, all interrupts masked: receive is drained
 * off the timer tick (the .poll hook). Works on VMware and QEMU's `-device vmxnet3`.
 */
#ifndef OWOS_VMXNET3_H
#define OWOS_VMXNET3_H

#include "nic.h"

extern const struct nic_ops vmxnet3_ops;

#endif /* OWOS_VMXNET3_H */

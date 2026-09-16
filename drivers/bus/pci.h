/* pci.h — PCI bus enumeration via legacy configuration mechanism #1. */
#ifndef OWOS_PCI_H
#define OWOS_PCI_H

#include <stdint.h>

void pci_init(void); /* scan the bus and cache what's found            */
void pci_list(void); /* print the device table (the `lspci` command)   */
int pci_count(void); /* number of devices found                        */

/* A detected display/VGA controller (PCI class 0x03), for the neofetch splash.
 */
struct pci_display {
  uint16_t vendor, device;
  const char *name; /* friendly model string ("" if unknown)        */
  unsigned mhz;     /* GPU max clock (Intel iGPU only; 0 if unknown) */
  int integrated;   /* 1 if an integrated GPU (Intel iGPU)           */
};

/* Fill out[] with up to max display controllers; returns the number found. */
int pci_displays(struct pci_display *out, int max);

/* One enumerated PCI device with a resolved friendly name, for
 * `lspci`. Layout MUST match struct pci_dev_info in user/lib/owos.h. */
struct pci_dev_info {
  char name[64]; /* "<Vendor> <Model>", or a class fallback        */
  uint16_t vendor, device;
  uint8_t bus, slot, func, class_code, subclass;
};
/* Fill *out with cached device #idx; returns 1 if it exists, 0 past the end. */
int pci_dev_info(int idx, struct pci_dev_info *out);

/* Read/write a 32-bit dword in a device's configuration space. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif /* OWOS_PCI_H */

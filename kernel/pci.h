/* pci.h — PCI bus enumeration via legacy configuration mechanism #1. */
#ifndef OWOS_PCI_H
#define OWOS_PCI_H

#include <stdint.h>

void     pci_init(void);   /* scan the bus and cache what's found            */
void     pci_list(void);   /* print the device table (the `lspci` command)   */
int      pci_count(void);  /* number of devices found                        */

/* A detected display/VGA controller (PCI class 0x03), for the neofetch splash. */
struct pci_display {
	uint16_t    vendor, device;
	const char *name;          /* friendly model string ("" if unknown)        */
};

/* Fill out[] with up to max display controllers; returns the number found.    */
int      pci_displays(struct pci_display *out, int max);

/* Read/write a 32-bit dword in a device's configuration space. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif /* OWOS_PCI_H */

/* pci.c — enumerate PCI devices through the 0xCF8/0xCFC config ports. */

#include "pci.h"
#include "io.h"
#include "console.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_MAX_DEVICES    32

struct pci_device {
	uint8_t  bus, slot, func;
	uint16_t vendor, device;
	uint8_t  class_code, subclass, prog_if;
};

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
	uint32_t address = 0x80000000u                 /* enable bit              */
	                 | ((uint32_t)bus  << 16)
	                 | ((uint32_t)slot << 11)
	                 | ((uint32_t)func << 8)
	                 | (offset & 0xFC);             /* dword-aligned           */
	outl(PCI_CONFIG_ADDRESS, address);
	return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
	uint32_t address = 0x80000000u
	                 | ((uint32_t)bus  << 16)
	                 | ((uint32_t)slot << 11)
	                 | ((uint32_t)func << 8)
	                 | (offset & 0xFC);
	outl(PCI_CONFIG_ADDRESS, address);
	outl(PCI_CONFIG_DATA, value);
}

static void record(uint8_t bus, uint8_t slot, uint8_t func)
{
	uint32_t id = pci_read32(bus, slot, func, 0x00);
	if ((id & 0xFFFF) == 0xFFFF)               /* 0xFFFF vendor = empty slot  */
		return;
	if (device_count >= PCI_MAX_DEVICES)
		return;

	uint32_t cls = pci_read32(bus, slot, func, 0x08);
	struct pci_device *d = &devices[device_count++];
	d->bus = bus;
	d->slot = slot;
	d->func = func;
	d->vendor = id & 0xFFFF;
	d->device = (id >> 16) & 0xFFFF;
	d->prog_if  = (cls >> 8)  & 0xFF;
	d->subclass = (cls >> 16) & 0xFF;
	d->class_code = (cls >> 24) & 0xFF;
}

void pci_init(void)
{
	device_count = 0;
	for (int bus = 0; bus < 256; bus++) {
		for (int slot = 0; slot < 32; slot++) {
			if ((pci_read32(bus, slot, 0, 0x00) & 0xFFFF) == 0xFFFF)
				continue;
			/* Probe all 8 functions only if the device is multi-function. */
			uint8_t header = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF;
			int funcs = (header & 0x80) ? 8 : 1;
			for (int f = 0; f < funcs; f++)
				record(bus, slot, f);
		}
	}
}

int pci_count(void)
{
	return device_count;
}

/* A friendly name for the PCI class/subclass pairs. Covers the full standard
 * base-class list (not just what QEMU exposes) so a real laptop's many
 * Intel-vendor devices we don't have an exact model for still read as e.g.
 * "Intel SMBus" instead of "Intel Unknown device". */
static const char *class_name(uint8_t cls, uint8_t sub)
{
	switch (cls) {
	case 0x00: return "Unclassified device";
	case 0x01:
		if (sub == 0x00) return "SCSI controller";
		if (sub == 0x01) return "IDE controller";
		if (sub == 0x02) return "Floppy controller";
		if (sub == 0x04) return "RAID controller";
		if (sub == 0x05) return "ATA controller";
		if (sub == 0x06) return "SATA controller";
		if (sub == 0x07) return "SAS controller";
		if (sub == 0x08) return "NVMe controller";
		return "Storage controller";
	case 0x02:
		if (sub == 0x00) return "Ethernet controller";
		if (sub == 0x80) return "Network controller";
		return "Network controller";
	case 0x03:
		if (sub == 0x00) return "VGA controller";
		return "Display controller";
	case 0x04:
		if (sub == 0x01) return "Audio controller";
		if (sub == 0x03) return "HD Audio controller";
		return "Multimedia controller";
	case 0x05:
		if (sub == 0x00) return "RAM controller";
		if (sub == 0x01) return "Flash memory controller";
		return "Memory controller";
	case 0x06:
		if (sub == 0x00) return "Host bridge";
		if (sub == 0x01) return "ISA bridge";
		if (sub == 0x04) return "PCI bridge";
		return "Bridge";
	case 0x07:
		if (sub == 0x80) return "Communication controller";
		return "Communication controller";
	case 0x08:
		if (sub == 0x05) return "SD Host controller";
		return "System peripheral";
	case 0x09: return "Input device controller";
	case 0x0A: return "Docking station";
	case 0x0B: return "Processor";
	case 0x0C:
		if (sub == 0x00) return "FireWire controller";
		if (sub == 0x03) return "USB controller";
		if (sub == 0x05) return "SMBus controller";
		return "Serial bus controller";
	case 0x0D: return "Wireless controller";
	case 0x0E: return "Intelligent controller";
	case 0x0F: return "Satellite communication controller";
	case 0x10: return "Encryption controller";
	case 0x11: return "Signal processing controller";
	case 0x12: return "Processing accelerator";
	case 0x13: return "Non-essential instrumentation";
	default: return "Unknown device";
	}
}

/* Friendly name for the display adapters QEMU and common hosts expose, keyed
 * by vendor (and device where it disambiguates). "" means "unknown model". */
static const char *display_name(uint16_t vendor, uint16_t device)
{
	switch (vendor) {
	case 0x1234:                                   /* QEMU/Bochs stdvga         */
		return "QEMU Standard VGA";
	case 0x1013:                                   /* Cirrus Logic              */
		return "Cirrus Logic GD5446";
	case 0x15ad:                                   /* VMware                    */
		return "VMware SVGA II";
	case 0x80ee:                                   /* VirtualBox                */
		return "VirtualBox VGA";
	case 0x1af4:                                   /* virtio                    */
		return "Virtio GPU";
	case 0x1b36:                                   /* Red Hat                   */
		return device == 0x0100 ? "QXL paravirtual GPU" : "Red Hat display";
	case 0x10de: return "NVIDIA GPU";
	case 0x1002: return "AMD/ATI GPU";
	case 0x8086: return "Intel Graphics";
	default:     return "";
	}
}

/* Short vendor name for the IDs that turn up on QEMU and common real hosts.
 * Used as a fallback when we don't recognise the exact device. */
static const char *vendor_name(uint16_t vendor)
{
	switch (vendor) {
	case 0x8086: return "Intel";
	case 0x10ec: return "Realtek";
	case 0x1234: return "QEMU";
	case 0x1b36: return "Red Hat";
	case 0x1af4: return "Virtio";
	case 0x1013: return "Cirrus Logic";
	case 0x15ad: return "VMware";
	case 0x80ee: return "VirtualBox";
	case 0x10de: return "NVIDIA";
	case 0x1002: return "AMD/ATI";
	case 0x1274: return "Ensoniq";
	case 0x1106: return "VIA";
	case 0x10ee: return "Xilinx";
	case 0x1d6b: return "Linux Foundation";
	case 0x1022: return "AMD";
	case 0x1000: return "LSI Logic";
	default:     return 0;
	}
}

/* Exact model name for the specific chips QEMU emulates (and a few common real
 * ones), keyed by vendor:device. Returns 0 when we only know the vendor/class. */
static const char *device_model(uint16_t vendor, uint16_t device)
{
	switch (((uint32_t)vendor << 16) | device) {
	/* Intel — the PIIX3/440FX chipset QEMU's default 'pc' machine exposes. */
	case 0x80861237: return "440FX - 82441FX PMC host bridge";
	case 0x80867000: return "82371SB PIIX3 ISA bridge";
	case 0x80867010: return "82371SB PIIX3 IDE controller";
	case 0x80867020: return "82371SB PIIX3 USB (UHCI)";
	case 0x80867113: return "82371AB/EB/MB PIIX4 ACPI";
	case 0x80862918: return "82801IB (ICH9) LPC bridge";
	case 0x80862922: return "82801IR/IO (ICH9) AHCI SATA";
	case 0x80862930: return "82801I (ICH9) SMBus";
	case 0x80862934: return "82801I (ICH9) USB UHCI";
	case 0x8086293a: return "82801I (ICH9) USB2 EHCI";
	case 0x808629c0: return "82G33/G31 Express DRAM controller";
	case 0x808610d3: return "82574L Gigabit Ethernet";
	case 0x8086100e: return "82540EM Gigabit Ethernet (e1000)";
	case 0x80862415: return "82801AA AC'97 Audio";
	/* Realtek. */
	case 0x10ec8139: return "RTL8139 Fast Ethernet";
	case 0x10ec8168: return "RTL8111/8168 Gigabit Ethernet";
	/* QEMU / Bochs. */
	case 0x12341111: return "Standard VGA";
	/* Red Hat / QEMU devices. */
	case 0x1b360001: return "QEMU PCI-PCI bridge";
	case 0x1b360010: return "QEMU NVMe controller";
	case 0x1b36000d: return "QEMU XHCI USB 3.0 controller";
	case 0x1b360100: return "QXL paravirtual GPU";
	/* Virtio. */
	case 0x1af41000: return "Virtio network device";
	case 0x1af41001: return "Virtio block device";
	case 0x1af41003: return "Virtio console";
	case 0x1af41004: return "Virtio SCSI";
	case 0x1af41005: return "Virtio RNG";
	case 0x1af41050: return "Virtio GPU";
	/* Display adapters from other hypervisors. */
	case 0x101300b8: return "GD 5446 SVGA";
	case 0x80eebeef: return "VBoxVGA";
	/* VMware — the 440BX-era chipset and virtual devices a VMware VM exposes. */
	case 0x80867190: return "440BX/ZX/DX host bridge";
	case 0x80867191: return "440BX/ZX/DX AGP bridge";
	case 0x80867110: return "82371AB/EB/MB PIIX4 ISA bridge";
	case 0x80867111: return "82371AB/EB/MB PIIX4 IDE controller";
	case 0x80867112: return "82371AB/EB/MB PIIX4 USB (UHCI)";
	case 0x15ad0405: return "SVGA II Adapter";
	case 0x15ad0710: return "SVGA Adapter";
	case 0x15ad0720: return "VMXNET Ethernet Controller";
	case 0x15ad07b0: return "VMXNET3 Ethernet Controller";
	case 0x15ad07c0: return "PVSCSI SCSI Controller";
	case 0x15ad0740: return "Virtual Machine Communication Interface";
	case 0x15ad0790: return "PCI bridge";
	case 0x15ad07a0: return "PCI Express Root Port";
	case 0x15ad0774: return "USB1.1 UHCI Controller";
	case 0x15ad0770: return "USB2 EHCI Controller";
	case 0x15ad0779: return "USB3 xHCI Controller";
	case 0x15ad0801: return "Virtual Machine Interface";
	/* AMD PCnet — VMware's default ("flexible"/vlance) NIC. */
	case 0x10222000: return "79c970 [PCnet32 LANCE] Ethernet";
	/* LSI Logic — VMware's default SCSI/SAS controllers. */
	case 0x10000030: return "53c1030 Fusion-MPT Dual Ultra320 SCSI";
	case 0x10000054: return "SAS1068 Fusion-MPT SAS";
	case 0x10000058: return "SAS1068E Fusion-MPT SAS";
	/* Ensoniq ES1371 — VMware's emulated sound card. */
	case 0x12741371: return "ES1371/ES1373 AudioPCI";
	/* Audio. */
	case 0x12745000: return "ES1370 AudioPCI";
	default:         return 0;
	}
}

int pci_displays(struct pci_display *out, int max)
{
	int n = 0;
	for (int i = 0; i < device_count && n < max; i++) {
		struct pci_device *d = &devices[i];
		if (d->class_code != 0x03)                 /* 0x03 = display controller */
			continue;
		out[n].vendor = d->vendor;
		out[n].device = d->device;
		out[n].name   = display_name(d->vendor, d->device);
		n++;
	}
	return n;
}

void pci_list(void)
{
	if (device_count == 0) {
		con_write("no PCI devices found\n");
		return;
	}
	con_write("bus:slot.fn vendor:device class description\n");
	for (int i = 0; i < device_count; i++) {
		struct pci_device *d = &devices[i];
		con_put_hex(d->bus, 2);  con_putc(':');
		con_put_hex(d->slot, 2); con_putc('.');
		con_put_hex(d->func, 1);
		con_write("    ");
		con_put_hex(d->vendor, 4); con_putc(':');
		con_put_hex(d->device, 4);
		con_write("   ");
		con_put_hex(d->class_code, 2);
		con_put_hex(d->subclass, 2);
		con_write("  ");
		/* Prefer the exact chip name; otherwise "<Vendor> <class>"; otherwise
		 * just the class. So a known card reads "Realtek RTL8139 Fast
		 * Ethernet", an unknown one "Intel Network controller". */
		const char *model  = device_model(d->vendor, d->device);
		const char *vendor = vendor_name(d->vendor);
		if (model) {
			if (vendor) { con_write(vendor); con_putc(' '); }
			con_write(model);
		} else if (vendor) {
			con_write(vendor); con_putc(' ');
			con_write(class_name(d->class_code, d->subclass));
		} else {
			con_write(class_name(d->class_code, d->subclass));
		}
		con_putc('\n');
	}
}

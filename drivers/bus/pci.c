/* pci.c — enumerate PCI devices through the 0xCF8/0xCFC config ports. */

#include "pci.h"
#include "console.h"
#include "io.h"
#include "paging.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_MAX_DEVICES 32

struct pci_device {
  uint8_t bus, slot, func;
  uint16_t vendor, device;
  uint8_t class_code, subclass, prog_if;
};

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
  uint32_t address = 0x80000000u /* enable bit              */
                     | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC); /* dword-aligned           */
  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
  uint32_t address =
      0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, value);
}

static void record(uint8_t bus, uint8_t slot, uint8_t func)
{
  uint32_t id = pci_read32(bus, slot, func, 0x00);
  if ((id & 0xFFFF) == 0xFFFF) /* 0xFFFF vendor = empty slot  */
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
  d->prog_if = (cls >> 8) & 0xFF;
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

int pci_count(void) { return device_count; }

/* A friendly name for the PCI class/subclass pairs. Covers the full standard
 * base-class list (not just what QEMU exposes) so a real laptop's many
 * Intel-vendor devices we don't have an exact model for still read as e.g.
 * "Intel SMBus" instead of "Intel Unknown device". */
static const char *class_name(uint8_t cls, uint8_t sub)
{
  switch (cls) {
  case 0x00:
    return "Unclassified device";
  case 0x01:
    if (sub == 0x00)
      return "SCSI controller";
    if (sub == 0x01)
      return "IDE controller";
    if (sub == 0x02)
      return "Floppy controller";
    if (sub == 0x04)
      return "RAID controller";
    if (sub == 0x05)
      return "ATA controller";
    if (sub == 0x06)
      return "SATA controller";
    if (sub == 0x07)
      return "SAS controller";
    if (sub == 0x08)
      return "NVMe controller";
    return "Storage controller";
  case 0x02:
    if (sub == 0x00)
      return "Ethernet controller";
    if (sub == 0x80)
      return "Network controller";
    return "Network controller";
  case 0x03:
    if (sub == 0x00)
      return "VGA controller";
    return "Display controller";
  case 0x04:
    if (sub == 0x01)
      return "Audio controller";
    if (sub == 0x03)
      return "HD Audio controller";
    return "Multimedia controller";
  case 0x05:
    if (sub == 0x00)
      return "RAM controller";
    if (sub == 0x01)
      return "Flash memory controller";
    return "Memory controller";
  case 0x06:
    if (sub == 0x00)
      return "Host bridge";
    if (sub == 0x01)
      return "ISA bridge";
    if (sub == 0x04)
      return "PCI bridge";
    return "Bridge";
  case 0x07:
    if (sub == 0x80)
      return "Communication controller";
    return "Communication controller";
  case 0x08:
    if (sub == 0x05)
      return "SD Host controller";
    return "System peripheral";
  case 0x09:
    return "Input device controller";
  case 0x0A:
    return "Docking station";
  case 0x0B:
    return "Processor";
  case 0x0C:
    if (sub == 0x00)
      return "FireWire controller";
    if (sub == 0x03)
      return "USB controller";
    if (sub == 0x05)
      return "SMBus controller";
    return "Serial bus controller";
  case 0x0D:
    return "Wireless controller";
  case 0x0E:
    return "Intelligent controller";
  case 0x0F:
    return "Satellite communication controller";
  case 0x10:
    return "Encryption controller";
  case 0x11:
    return "Signal processing controller";
  case 0x12:
    return "Processing accelerator";
  case 0x13:
    return "Non-essential instrumentation";
  default:
    return "Unknown device";
  }
}

/* Friendly name for the display adapters QEMU and common hosts expose, keyed
 * by vendor (and device where it disambiguates). "" means "unknown model". */
/* Specific Intel integrated-GPU model names, keyed by PCI device id, so the
 * splash reads "Intel HD Graphics 520" (like fastfetch) rather than a generic
 * "Intel Graphics". Centred on the Haswell..Tiger Lake parts a laptop is likely
 * to carry; anything unrecognised falls back to the generic name. */
static const char *intel_gpu_name(uint16_t device)
{
  switch (device) {
  /* Haswell / Broadwell (Gen7.5 / Gen8). */
  case 0x0416:
    return "Intel HD Graphics 4600";
  case 0x0A16:
    return "Intel HD Graphics 4400";
  case 0x0A26:
    return "Intel HD Graphics 5000";
  case 0x0D26:
    return "Intel Iris Pro Graphics 5200";
  case 0x1616:
    return "Intel HD Graphics 5500";
  case 0x161E:
    return "Intel HD Graphics 5300";
  case 0x1626:
    return "Intel HD Graphics 6000";
  case 0x162B:
    return "Intel Iris Graphics 6100";
  /* Skylake (Gen9). */
  case 0x1902:
  case 0x1906:
  case 0x190B:
    return "Intel HD Graphics 510";
  case 0x191E:
    return "Intel HD Graphics 515";
  case 0x1916:
  case 0x1921:
    return "Intel HD Graphics 520";
  case 0x1912:
  case 0x191B:
    return "Intel HD Graphics 530";
  case 0x1926:
    return "Intel Iris Graphics 540";
  case 0x1927:
    return "Intel Iris Graphics 550";
  case 0x192B:
    return "Intel Iris Graphics 555";
  /* Kaby Lake / Coffee Lake / Comet Lake (Gen9.5). */
  case 0x5906:
    return "Intel HD Graphics 610";
  case 0x5916:
  case 0x5921:
    return "Intel HD Graphics 620";
  case 0x5917:
    return "Intel UHD Graphics 620";
  case 0x591E:
    return "Intel HD Graphics 615";
  case 0x5912:
  case 0x591B:
    return "Intel HD Graphics 630";
  case 0x5926:
    return "Intel Iris Plus Graphics 640";
  case 0x5927:
    return "Intel Iris Plus Graphics 650";
  case 0x3E90:
  case 0x3E93:
    return "Intel UHD Graphics 610";
  case 0x3E91:
  case 0x3E92:
  case 0x3E98:
  case 0x3E9B:
  case 0x9BC5:
  case 0x9BC8:
    return "Intel UHD Graphics 630";
  case 0x3EA0:
    return "Intel UHD Graphics 620";
  case 0x3EA5:
    return "Intel Iris Plus Graphics 655";
  /* Ice Lake / Tiger Lake (Gen11 / Gen12). */
  case 0x8A52:
  case 0x8A53:
    return "Intel Iris Plus Graphics";
  case 0x9A49:
  case 0x9A40:
    return "Intel Iris Xe Graphics";
  case 0x9A78:
    return "Intel UHD Graphics";
  default:
    return "Intel Graphics";
  }
}

/* The Intel iGPU's maximum render clock, in MHz, read from the GT RP_STATE_CAP
 * register in BAR0 MMIO — the GPU analogue of the CPU's cpuid clock.
 * Best-effort and real-hardware-only (QEMU's VGA has no GT block): returns 0
 * when it can't be read, so the splash simply omits it. RP_STATE_CAP is a
 * static capability register in the always-on power well, so it reads without
 * forcewake even when the GPU is in RC6.
 *
 * The byte-0 field is RP0 (the max P-state). For Gen6..Gen8 the displayed clock
 * is rp0*50 MHz; for Gen9 big-core (Skylake/Kaby/Coffee) i915 scales rp0 by 3
 * then divides the 50 MHz step by 3, so the factors cancel and it is *also*
 * rp0*50 — a single formula covers the parts this targets. (Gen9-LP atoms
 * differ; they're not a target.) */
static unsigned intel_gpu_mhz(uint8_t bus, uint8_t slot, uint8_t func)
{
  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
  if (((bar0 >> 1) & 0x3) == 0x2 && /* 64-bit BAR above 4 GiB?    */
      pci_read32(bus, slot, func, 0x14) != 0)
    return 0;
  uint32_t bar = bar0 & ~0xFu;
  if (!bar)
    return 0;

  /* RP_STATE_CAP lives at MCHBAR_MIRROR_BASE_SNB (0x140000) + 0x5998; map
   * enough of BAR0 to reach it, uncached, one-shot (like edid.c's GMBUS map).
   */
  if (paging_identity_map(bar, 0x180000) != 0)
    return 0;
  paging_mark_uc(bar, 0x180000);

  volatile uint32_t *cap = (volatile uint32_t *)(bar + 0x145998);
  uint32_t rp0 = *cap & 0xFF;
  return rp0 * 50u;
}

static const char *display_name(uint16_t vendor, uint16_t device)
{
  switch (vendor) {
  case 0x1234: /* QEMU/Bochs stdvga         */
    return "QEMU Standard VGA";
  case 0x1013: /* Cirrus Logic              */
    return "Cirrus Logic GD5446";
  case 0x15ad: /* VMware                    */
    return "VMware SVGA II";
  case 0x80ee: /* VirtualBox                */
    return "VirtualBox VGA";
  case 0x1af4: /* virtio                    */
    return "Virtio GPU";
  case 0x1b36: /* Red Hat                   */
    return device == 0x0100 ? "QXL paravirtual GPU" : "Red Hat display";
  case 0x10de:
    return "NVIDIA GPU";
  case 0x1002:
    return "AMD/ATI GPU";
  case 0x8086:
    return intel_gpu_name(device);
  default:
    return "";
  }
}

/* Short vendor name for the IDs that turn up on QEMU and common real hosts.
 * Used as a fallback when we don't recognise the exact device. */
static const char *vendor_name(uint16_t vendor)
{
  switch (vendor) {
  case 0x8086:
    return "Intel";
  case 0x10ec:
    return "Realtek";
  case 0x1234:
    return "QEMU";
  case 0x1b36:
    return "Red Hat";
  case 0x1af4:
    return "Virtio";
  case 0x1013:
    return "Cirrus Logic";
  case 0x15ad:
    return "VMware";
  case 0x80ee:
    return "VirtualBox";
  case 0x10de:
    return "NVIDIA";
  case 0x1002:
    return "AMD/ATI";
  case 0x1274:
    return "Ensoniq";
  case 0x1106:
    return "VIA";
  case 0x10ee:
    return "Xilinx";
  case 0x1d6b:
    return "Linux Foundation";
  case 0x1022:
    return "AMD";
  case 0x1000:
    return "LSI Logic";
  default:
    return 0;
  }
}

/* Exact model name for the specific chips QEMU emulates (and a few common real
 * ones), keyed by vendor:device. Returns 0 when we only know the vendor/class.
 */
static const char *device_model(uint16_t vendor, uint16_t device)
{
  switch (((uint32_t)vendor << 16) | device) {
  /* Intel — the PIIX3/440FX chipset QEMU's default 'pc' machine exposes. */
  case 0x80861237:
    return "440FX - 82441FX PMC host bridge";
  case 0x80867000:
    return "82371SB PIIX3 ISA bridge";
  case 0x80867010:
    return "82371SB PIIX3 IDE controller";
  case 0x80867020:
    return "82371SB PIIX3 USB (UHCI)";
  case 0x80867113:
    return "82371AB/EB/MB PIIX4 ACPI";
  case 0x80862918:
    return "82801IB (ICH9) LPC bridge";
  case 0x80862922:
    return "82801IR/IO (ICH9) AHCI SATA";
  case 0x80862930:
    return "82801I (ICH9) SMBus";
  case 0x80862934:
    return "82801I (ICH9) USB UHCI";
  case 0x8086293a:
    return "82801I (ICH9) USB2 EHCI";
  case 0x808629c0:
    return "82G33/G31 Express DRAM controller";
  case 0x808610d3:
    return "82574L Gigabit Ethernet";
  case 0x8086100e:
    return "82540EM Gigabit Ethernet (e1000)";
  case 0x808610c9:
    return "82576 Gigabit Network Connection (igb)";
  case 0x80861533:
    return "I210 Gigabit Network Connection";
  case 0x80861539:
    return "I211 Gigabit Network Connection";
  case 0x80862415:
    return "82801AA AC'97 Audio";
  case 0x80862668:
    return "82801G (ICH7) HD Audio"; /* QEMU -device intel-hda     */
  case 0x8086293e:
    return "82801I (ICH9) HD Audio"; /* QEMU -device ich9-intel-hda */
  /* Realtek. */
  case 0x10ec8139:
    return "RTL8139 Fast Ethernet";
  case 0x10ec8168:
    return "RTL8111/8168 Gigabit Ethernet";
  /* QEMU / Bochs. */
  case 0x12341111:
    return "Standard VGA";
  /* Red Hat / QEMU devices. */
  case 0x1b360001:
    return "QEMU PCI-PCI bridge";
  case 0x1b360010:
    return "QEMU NVMe controller";
  case 0x1b36000d:
    return "QEMU XHCI USB 3.0 controller";
  case 0x1b360100:
    return "QXL paravirtual GPU";
  /* Virtio. */
  case 0x1af41000:
    return "Virtio network device";
  case 0x1af41001:
    return "Virtio block device";
  case 0x1af41003:
    return "Virtio console";
  case 0x1af41004:
    return "Virtio SCSI";
  case 0x1af41005:
    return "Virtio RNG";
  case 0x1af41050:
    return "Virtio GPU";
  /* Display adapters from other hypervisors. */
  case 0x101300b8:
    return "GD 5446 SVGA";
  case 0x80eebeef:
    return "VBoxVGA";
  /* VMware — the 440BX-era chipset and virtual devices a VMware VM exposes. */
  case 0x80867190:
    return "440BX/ZX/DX host bridge";
  case 0x80867191:
    return "440BX/ZX/DX AGP bridge";
  case 0x80867110:
    return "82371AB/EB/MB PIIX4 ISA bridge";
  case 0x80867111:
    return "82371AB/EB/MB PIIX4 IDE controller";
  case 0x80867112:
    return "82371AB/EB/MB PIIX4 USB (UHCI)";
  case 0x15ad0405:
    return "SVGA II Adapter";
  case 0x15ad0710:
    return "SVGA Adapter";
  case 0x15ad0720:
    return "VMXNET Ethernet Controller";
  case 0x15ad07b0:
    return "VMXNET3 Ethernet Controller";
  case 0x15ad07c0:
    return "PVSCSI SCSI Controller";
  case 0x15ad0740:
    return "Virtual Machine Communication Interface";
  case 0x15ad0790:
    return "PCI bridge";
  case 0x15ad07a0:
    return "PCI Express Root Port";
  case 0x15ad0774:
    return "USB1.1 UHCI Controller";
  case 0x15ad0770:
    return "USB2 EHCI Controller";
  case 0x15ad0779:
    return "USB3 xHCI Controller";
  case 0x15ad0801:
    return "Virtual Machine Interface";
  /* AMD PCnet — VMware's default ("flexible"/vlance) NIC. */
  case 0x10222000:
    return "79c970 [PCnet32 LANCE] Ethernet";
  /* LSI Logic — VMware's default SCSI/SAS controllers. */
  case 0x10000030:
    return "53c1030 Fusion-MPT Dual Ultra320 SCSI";
  case 0x10000054:
    return "SAS1068 Fusion-MPT SAS";
  case 0x10000058:
    return "SAS1068E Fusion-MPT SAS";
  /* Ensoniq ES1371 — VMware's emulated sound card. */
  case 0x12741371:
    return "ES1371/ES1373 AudioPCI";
  /* Audio. */
  case 0x12745000:
    return "ES1370 AudioPCI";
  default:
    return 0;
  }
}

int pci_displays(struct pci_display *out, int max)
{
  int n = 0;
  for (int i = 0; i < device_count && n < max; i++) {
    struct pci_device *d = &devices[i];
    if (d->class_code != 0x03) /* 0x03 = display controller */
      continue;
    out[n].vendor = d->vendor;
    out[n].device = d->device;
    out[n].name = display_name(d->vendor, d->device);
    out[n].integrated = (d->vendor == 0x8086); /* Intel iGPU */
    /* Map BAR0 + probe the GT clock once; cache it (the answer is fixed for
     * the session, and we don't want to remap MMIO on every neofetch). */
    out[n].mhz = 0;
    if (d->vendor == 0x8086) {
      static int cached_mhz = -1;
      if (cached_mhz < 0)
        cached_mhz = (int)intel_gpu_mhz(d->bus, d->slot, d->func);
      out[n].mhz = (unsigned)cached_mhz;
    }
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
    con_printf("%02x:%02x.%01x    %04x:%04x   %02x%02x  ", d->bus, d->slot, d->func, d->vendor, d->device,
               d->class_code, d->subclass);
    /* Prefer the exact chip name; otherwise "<Vendor> <class>"; otherwise
     * just the class. So a known card reads "Realtek RTL8139 Fast
     * Ethernet", an unknown one "Intel Network controller". */
    const char *model = device_model(d->vendor, d->device);
    const char *vendor = vendor_name(d->vendor);
    if (model) {
      if (vendor) {
        con_printf("%s ", vendor);
      }
      con_write(model);
    } else if (vendor) {
      con_printf("%s %s", vendor, class_name(d->class_code, d->subclass));
    } else {
      con_write(class_name(d->class_code, d->subclass));
    }
    con_putc('\n');
  }
}

/* Bounded string append: copy `s` at `p` without passing `end`; returns the new
 * end pointer. */
static char *pci_cat(char *p, char *end, const char *s)
{
  while (s && *s && p < end)
    *p++ = *s++;
  return p;
}

/* Fill *out with cached device #idx, resolving the same friendly name pci_list
 * prints. Returns 1 if it exists, 0 past the end. The data is cached from boot,
 * so this touches no PCI config space. For `lspci` over SYS_PCIDEV. */
int pci_dev_info(int idx, struct pci_dev_info *out)
{
  if (idx < 0 || idx >= device_count)
    return 0;
  struct pci_device *d = &devices[idx];
  out->bus = d->bus;
  out->slot = d->slot;
  out->func = d->func;
  out->vendor = d->vendor;
  out->device = d->device;
  out->class_code = d->class_code;
  out->subclass = d->subclass;

  const char *model = device_model(d->vendor, d->device);
  const char *vendor = vendor_name(d->vendor);
  char *p = out->name, *e = out->name + sizeof out->name - 1;
  if (model) {
    if (vendor) {
      p = pci_cat(p, e, vendor);
      p = pci_cat(p, e, " ");
    }
    p = pci_cat(p, e, model);
  } else if (vendor) {
    p = pci_cat(p, e, vendor);
    p = pci_cat(p, e, " ");
    p = pci_cat(p, e, class_name(d->class_code, d->subclass));
  } else {
    p = pci_cat(p, e, class_name(d->class_code, d->subclass));
  }
  *p = '\0';
  return 1;
}

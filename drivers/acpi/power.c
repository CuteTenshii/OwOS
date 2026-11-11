/* power.c: ACPI, QEMU-port and 8042/triple-fault power off, reset and S3 sleep. */

#include "power.h"
#include "backlight.h"
#include "io.h"
#include "irq.h"
#include "paging.h"
#include "timer.h"
#include <stdbool.h>
#include <stdint.h>

/* ACPI shutdown: tables sit in the identity-mapped low 16 MiB; write SLP_TYPa|SLP_EN from _S5 to PM1 CNT. */

struct rsdp {
  char signature[8]; /* "RSD PTR " */
  uint8_t checksum;
  char oemid[6];
  uint8_t revision; /* 0 = ACPI 1.0 (RSDT only); >=2 adds XSDT */
  uint32_t rsdt_addr;
  /* ACPI 2.0+ extension; UEFI often zeroes rsdt_addr and fills only this XSDT pointer. */
  uint32_t length;
  uint64_t xsdt_addr;
  uint8_t ext_checksum;
  uint8_t reserved[3];
} __attribute__((packed));

struct sdt_header {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oemid[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

/* FADT prefix up to the PM1 control blocks; offsets per ACPI spec. */
struct fadt {
  struct sdt_header header;     /* 0  */
  uint32_t firmware_ctrl;       /* 36 */
  uint32_t dsdt;                /* 40 */
  uint8_t reserved0;            /* 44 */
  uint8_t preferred_pm_profile; /* 45 */
  uint16_t sci_int;             /* 46 */
  uint32_t smi_cmd;             /* 48 */
  uint8_t acpi_enable;          /* 52 */
  uint8_t acpi_disable;         /* 53 */
  uint8_t s4bios_req;           /* 54 */
  uint8_t pstate_cnt;           /* 55 */
  uint32_t pm1a_evt_blk;        /* 56 */
  uint32_t pm1b_evt_blk;        /* 60 */
  uint32_t pm1a_cnt_blk;        /* 64 */
  uint32_t pm1b_cnt_blk;        /* 68 */
} __attribute__((packed));

#define SLP_EN (1 << 13) /* PM1_CNT sleep-enable bit */
#define SCI_EN (1 << 0)  /* PM1a_CNT: ACPI mode is on */

static bool sig4(const char *a, const char *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

/* Scan a physical range, 16-byte aligned, for the RSDP anchor string. */
static struct rsdp *scan_rsdp(uint32_t start, uint32_t end)
{
  for (uint32_t p = start; p < end; p += 16) {
    struct rsdp *r = (struct rsdp *)(uintptr_t)p;
    if (r->signature[0] == 'R' && r->signature[1] == 'S' && r->signature[2] == 'D' && r->signature[3] == ' ' &&
        r->signature[4] == 'P' && r->signature[5] == 'T' && r->signature[6] == 'R' && r->signature[7] == ' ')
      return r;
  }
  return 0;
}

/* RSDP copy from the multiboot2 ACPI tag; on UEFI the BIOS-area scan finds nothing. */
static struct rsdp boot_rsdp;
static bool boot_rsdp_valid;

void acpi_set_rsdp(const void *rsdp_copy)
{
  const struct rsdp *src = rsdp_copy;
  /* Ignore anything that isn't really an RSDP. */
  if (!(src->signature[0] == 'R' && src->signature[1] == 'S' && src->signature[2] == 'D' && src->signature[3] == ' '))
    return;
  /* Don't let a type-14 (1.0) tag clobber a captured 2.0 RSDP; only 2.0 carries the XSDT. */
  if (boot_rsdp_valid && boot_rsdp.revision >= 2 && src->revision < 2)
    return;

  const uint8_t *s = rsdp_copy;
  uint8_t *d = (uint8_t *)&boot_rsdp;
  /* A 2.0 RSDP is 36 bytes, a 1.0 one only 20. */
  uint32_t len = src->revision >= 2 ? sizeof boot_rsdp : 20;
  for (uint32_t i = 0; i < len; i++)
    d[i] = s[i];
  /* Reachable if it names a root table (RSDT or XSDT). */
  boot_rsdp_valid = boot_rsdp.rsdt_addr != 0 || (boot_rsdp.revision >= 2 && boot_rsdp.xsdt_addr != 0);
}

static struct rsdp *find_rsdp(void)
{
  /* Prefer the loader-provided copy; absent on legacy boot (boot_rsdp_valid == 0). */
  if (boot_rsdp_valid)
    return &boot_rsdp;

  /* The EBDA segment pointer is the real-mode word at 0x40E. */
  uintptr_t ebda_addr = 0x40E;
  uint16_t ebda_word;
  __asm__ volatile("movw (%[reg]), %[out]" : [out] "=r"(ebda_word) : [reg] "r"(ebda_addr) : "memory");
  uint32_t ebda = (uint32_t)ebda_word << 4;
  struct rsdp *r = 0;
  if (ebda)
    r = scan_rsdp(ebda, ebda + 1024);
  if (!r)
    r = scan_rsdp(0xE0000, 0x100000);
  return r;
}

/* Map the root table (XSDT if populated, else RSDT); *count = entries, *xsdt = 1 for 8-byte entries. */
static bool find_root_sdt(struct sdt_header **hdr, uint32_t *count, bool *xsdt)
{
  struct rsdp *rsdp = find_rsdp();
  if (!rsdp)
    return false;

  uint32_t phys;
  bool is_xsdt;
  if (rsdp->revision >= 2 && rsdp->xsdt_addr && rsdp->xsdt_addr < 0x100000000ull) {
    phys = (uint32_t)rsdp->xsdt_addr;
    is_xsdt = true;
  } else {
    phys = rsdp->rsdt_addr;
    is_xsdt = false;
  }
  if (!phys)
    return false;

  struct sdt_header *h = (struct sdt_header *)(uintptr_t)phys;
  if (paging_identity_map(phys, sizeof(*h)) != 0)
    return false;
  if (!sig4(h->signature, is_xsdt ? "XSDT" : "RSDT"))
    return false;
  if (paging_identity_map(phys, h->length) != 0)
    return false;

  *hdr = h;
  *count = (h->length - sizeof(*h)) / (is_xsdt ? 8 : 4);
  *xsdt = is_xsdt;
  return true;
}

/* Physical address of the i-th root entry; truncation to 32 bits is safe (tables are below 4 GiB). */
static uint32_t root_entry(struct sdt_header *root, uint32_t i, bool xsdt)
{
  uint8_t *base = (uint8_t *)root + sizeof(struct sdt_header);
  if (xsdt)
    return (uint32_t)(*(uint64_t *)(base + (uint32_t)i * 8));
  return *(uint32_t *)(base + (uint32_t)i * 4);
}

/* DSDT address from the FADT: prefer the 64-bit X_DSDT (offset 140), since UEFI often zeroes dsdt. */
static uint32_t fadt_dsdt_phys(struct fadt *f)
{
  if (f->header.length >= 148) {
    uint64_t x = *(uint64_t *)((uint8_t *)f + 140);
    if (x && x < 0x100000000ull)
      return (uint32_t)x;
  }
  return f->dsdt;
}

/* Read SLP_TYPa/b from the DSDT package `name` ("_S5_", "_S3_"), stored already shifted into PM1_CNT position. */
static bool parse_sleep(struct fadt *f, const char *name, uint16_t *slp_a, uint16_t *slp_b)
{
  uint32_t dphys = fadt_dsdt_phys(f);
  struct sdt_header *dsdt = (struct sdt_header *)(uintptr_t)dphys;
  /* Map the header, then the whole table once its length is known. */
  if (paging_identity_map(dphys, sizeof(*dsdt)) != 0)
    return false;
  if (!dsdt || !sig4(dsdt->signature, "DSDT"))
    return false;
  if (paging_identity_map(dphys, dsdt->length) != 0)
    return false;

  uint8_t *p = (uint8_t *)dsdt + sizeof(*dsdt);
  uint8_t *end = (uint8_t *)dsdt + dsdt->length;
  for (; p < end - 4; p++) {
    if (sig4((char *)p, name))
      break;
  }
  if (p >= end - 4)
    return false;

  /* Require NameOp (0x08), optionally after root '\\' (0x5C), then the name, then PackageOp (0x12). */
  if (!((*(p - 1) == 0x08) || (*(p - 2) == 0x08 && *(p - 1) == 0x5C)) || *(p + 4) != 0x12)
    return false;

  p += 5; /* skip the name and the PackageOp byte */
  if (p >= end)
    return false;
  p += ((*p & 0xC0) >> 6) + 2; /* skip PkgLength + element count       */
  if (p >= end)
    return false;

  if (*p == 0x0A) { /* BytePrefix before SLP_TYPa           */
    p++;
    if (p >= end)
      return false;
  }
  *slp_a = (uint16_t)(*p++) << 10;
  if (p >= end)
    return false;

  if (*p == 0x0A) { /* BytePrefix before SLP_TYPb           */
    p++;
    if (p >= end)
      return false;
  }
  *slp_b = (uint16_t)(*p) << 10;
  return true;
}

/* Map a primary ACPI table by signature in full; 0 if ACPI or the table is absent. */
void *acpi_find_table(const char *sig)
{
  struct sdt_header *root;
  uint32_t count;
  bool xsdt;
  if (!find_root_sdt(&root, &count, &xsdt))
    return 0;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t phys = root_entry(root, i, xsdt);
    if (paging_identity_map(phys, sizeof(struct sdt_header)) != 0)
      continue;
    struct sdt_header *h = (struct sdt_header *)(uintptr_t)phys;
    if (sig4(h->signature, sig)) {
      if (paging_identity_map(phys, h->length) != 0)
        return 0;
      return h;
    }
  }
  return 0;
}

/* The FADT ("FACP"), fully mapped so fadt_dsdt_phys() can read X_DSDT at offset 140. */
static struct fadt *find_fadt(void) { return (struct fadt *)acpi_find_table("FACP"); }

/* Map one located SDT and scan its AML body for a byte sequence; reads stay inside the mapped span. */
static bool sdt_contains(struct sdt_header *h, const char *sig, const uint8_t *needle, uint32_t n)
{
  if (!h || paging_identity_map((uint32_t)(uintptr_t)h, sizeof(*h)) != 0)
    return false;
  if (!sig4(h->signature, sig) || h->length <= sizeof(*h) || n == 0)
    return false;
  if (paging_identity_map((uint32_t)(uintptr_t)h, h->length) != 0)
    return false;

  const uint8_t *body = (const uint8_t *)h + sizeof(*h);
  uint32_t len = h->length - sizeof(*h);
  if (len < n)
    return false;
  for (uint32_t i = 0; i + n <= len; i++) {
    uint32_t j = 0;
    while (j < n && body[i + j] == needle[j])
      j++;
    if (j == n)
      return true;
  }
  return false;
}

/* Scan the DSDT and every SSDT for a byte sequence; SSDTs matter since firmware may declare the battery there. */
bool acpi_aml_contains(const uint8_t *needle, uint32_t n)
{
  struct sdt_header *root;
  uint32_t count;
  bool xsdt;
  if (!find_root_sdt(&root, &count, &xsdt))
    return false;

  /* The DSDT hangs off the FADT, not the root table (via X_DSDT when present). */
  struct fadt *f = find_fadt();
  if (f && sdt_contains((struct sdt_header *)(uintptr_t)fadt_dsdt_phys(f), "DSDT", needle, n))
    return true;

  /* Every other table is a root-table entry; scan the SSDTs. */
  for (uint32_t i = 0; i < count; i++) {
    uint32_t phys = root_entry(root, i, xsdt);
    if (paging_identity_map(phys, sizeof(struct sdt_header)) != 0)
      continue;
    if (sdt_contains((struct sdt_header *)(uintptr_t)phys, "SSDT", needle, n))
      return true;
  }
  return false;
}

/* Pass each AML block (DSDT, then SSDTs) to fn as (body, len) past the 36-byte header; mirrors acpi_aml_contains. */
void acpi_for_each_aml(void (*fn)(const uint8_t *body, uint32_t len, void *ctx), void *ctx)
{
  struct sdt_header *root;
  uint32_t count;
  bool xsdt;
  if (!find_root_sdt(&root, &count, &xsdt))
    return;

  struct fadt *f = find_fadt();
  if (f) {
    uint32_t dphys = fadt_dsdt_phys(f);
    struct sdt_header *d = (struct sdt_header *)(uintptr_t)dphys;
    if (paging_identity_map(dphys, sizeof(*d)) == 0 && sig4(d->signature, "DSDT") &&
        paging_identity_map(dphys, d->length) == 0 && d->length > sizeof(*d))
      fn((const uint8_t *)d + sizeof(*d), d->length - sizeof(*d), ctx);
  }

  for (uint32_t i = 0; i < count; i++) {
    uint32_t phys = root_entry(root, i, xsdt);
    struct sdt_header *h = (struct sdt_header *)(uintptr_t)phys;
    if (paging_identity_map(phys, sizeof(*h)) != 0)
      continue;
    if (!sig4(h->signature, "SSDT"))
      continue;
    if (paging_identity_map(phys, h->length) != 0)
      continue;
    if (h->length > sizeof(*h))
      fn((const uint8_t *)h + sizeof(*h), h->length - sizeof(*h), ctx);
  }
}

/* Enter ACPI mode if needed so PM1 CNT is live (QEMU already is). */
static void acpi_ensure_enabled(struct fadt *f)
{
  if (f->smi_cmd && f->acpi_enable && !(inw((uint16_t)f->pm1a_cnt_blk) & SCI_EN)) {
    outb((uint16_t)f->smi_cmd, f->acpi_enable);
    for (int i = 0; i < 300000; i++) {
      if (inw((uint16_t)f->pm1a_cnt_blk) & SCI_EN)
        break;
      io_wait();
    }
  }
}

/* Returns only if ACPI shutdown could not be performed. */
static void acpi_off(void)
{
  struct fadt *f = find_fadt();
  if (!f)
    return;

  uint16_t slp_a, slp_b;
  if (!parse_sleep(f, "_S5_", &slp_a, &slp_b))
    return;

  acpi_ensure_enabled(f);

  outw((uint16_t)f->pm1a_cnt_blk, slp_a | SLP_EN);
  if (f->pm1b_cnt_blk)
    outw((uint16_t)f->pm1b_cnt_blk, slp_b | SLP_EN);
}

void power_off(void)
{
  /* Real ACPI first, the only path VMware and bare metal honour. */
  acpi_off();

  /* Emulator power-off ports: QEMU >= 2.0 0x604, old QEMU/Bochs 0xB004, VirtualBox 0x4004. */
  outw(0x604, 0x2000);  /* QEMU            */
  outw(0xB004, 0x2000); /* Bochs / old QEMU */
  outw(0x4004, 0x3400); /* VirtualBox      */
}

/* ACPI reset register: FADT RESET_REG (GAS, offset 116) and RESET_VALUE (128), valid if flags bit 10 is set. */
static void acpi_reset(void)
{
  struct fadt *f = find_fadt();
  if (!f || f->header.length < 129)
    return;

  uint32_t flags = *(uint32_t *)((uint8_t *)f + 112);
  if (!(flags & (1u << 10))) /* RESET_REG_SUP */
    return;

  /* GAS layout: [0]=address-space id, [1..3]=width/offset/access, [4..11]=addr. */
  uint8_t *gas = (uint8_t *)f + 116;
  uint8_t space = gas[0]; /* 0 = system memory, 1 = system I/O */
  uint64_t addr = *(uint64_t *)(gas + 4);
  uint8_t value = *((uint8_t *)f + 128);
  if (!addr)
    return;

  if (space == 1) { /* system I/O space (the common case) */
    outb((uint16_t)addr, value);
  } else if (space == 0) { /* memory-mapped reset register */
    if (addr < 0x100000000ull && paging_identity_map((uint32_t)addr, 1) == 0)
      *(volatile uint8_t *)(uintptr_t)(uint32_t)addr = value;
  }
}

void power_reset(void)
{
  /* Firmware-declared ACPI reset first; the most reliable path on UEFI hardware. */
  acpi_reset();

  /* PCI reset control at 0xCF9: bit 1 SYS_RST (full reset), bit 2 RST_CPU, written in two steps. */
  {
    uint8_t v = inb(0xCF9) & ~0x06u;
    outb(0xCF9, v | 0x02); /* arm: SYS_RST */
    io_wait();
    outb(0xCF9, v | 0x06); /* fire: SYS_RST | RST_CPU */
    io_wait();
  }

  /* Bounded 8042 wait: with no 8042, 0x64 reads 0xFF so an unbounded loop hangs. Then pulse 0xFE regardless. */
  for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
    io_wait();
  outb(0x64, 0xFE);

  /* Last resort: a null IDT makes the next interrupt reset the CPU. */
  struct {
    uint16_t limit;
    uint32_t base;
  } __attribute__((packed)) null_idt = {0, 0};
  __asm__ volatile("lidt %0; int3" : : "m"(null_idt));
}

/* ACPI S3: save CPU state, set the FACS wake vector to our trampoline (s3_wake.S), write S3 to PM1_CNT. */
/* Restores the CPU only; real-hardware devices lose power across S3 and are not re-inited yet. */
/* Trampoline physical landing address; must match .set TRAMP in s3_wake.S (free RAM, identity-mapped). */
#define S3_TRAMPOLINE_PHYS 0x8000u
extern uint8_t s3_trampoline_start[];
extern uint8_t s3_trampoline_end[];
extern void s3_enter(uint32_t pm1a_port, uint32_t pm1b_port, uint32_t slp_a_value, uint32_t slp_b_value);

/* Map and validate the FACS (FIRMWARE_CTRL at offset 36, or X_FIRMWARE_CTRL at 132); 0 on failure. */
static uint8_t *find_facs(struct fadt *f)
{
  uint32_t phys = f->firmware_ctrl;
  if (f->header.length >= 140) {
    uint64_t x = *(uint64_t *)((uint8_t *)f + 132);
    if (x && x < 0x100000000ull)
      phys = (uint32_t)x;
  }
  if (!phys || paging_identity_map(phys, 64) != 0)
    return 0;
  uint8_t *facs = (uint8_t *)(uintptr_t)phys;
  if (!sig4((char *)facs, "FACS"))
    return 0;
  return facs;
}

/* Read-only probe: FADT, a DSDT _S3 package and a reachable FACS gate the Sleep UI item. */
bool power_can_sleep(void)
{
  struct fadt *f = find_fadt();
  if (!f)
    return false;
  uint16_t a, b;
  if (!parse_sleep(f, "_S3_", &a, &b))
    return false;
  return find_facs(f) != 0;
}

/* Enter ACPI S3; returns after resume, or at once if S3 can't be done. */
void power_sleep(void)
{
  struct fadt *f = find_fadt();
  if (!f)
    return;

  uint16_t slp_a, slp_b;
  if (!parse_sleep(f, "_S3_", &slp_a, &slp_b))
    return; /* firmware doesn't advertise S3 */

  uint8_t *facs = find_facs(f);
  if (!facs)
    return;

  acpi_ensure_enabled(f);

  /* Copy the trampoline to low memory; set the 32-bit waking vector (FACS offset 12), zero the 64-bit one (24). */
  uint32_t len = (uint32_t)(s3_trampoline_end - s3_trampoline_start);
  uint8_t *dst = (uint8_t *)S3_TRAMPOLINE_PHYS;
  for (uint32_t i = 0; i < len; i++)
    dst[i] = s3_trampoline_start[i];

  *(volatile uint32_t *)(facs + 12) = S3_TRAMPOLINE_PHYS;
  if (facs[32] >= 1) /* FACS version >= 1 has the 64-bit field */
    *(volatile uint64_t *)(facs + 24) = 0;

  /* Ensure the trampoline and saved state are in RAM before power drops. */
  __asm__ volatile("wbinvd");

  /* Snapshot PIC masks: the wake POST resets the 8259. */
  uint8_t pic_mask1 = inb(0x21);
  uint8_t pic_mask2 = inb(0xA1);

  /* Asm snapshots the CPU and writes SLP_TYP|SLP_EN; control returns after the firmware re-enters. */
  s3_enter(f->pm1a_cnt_blk, f->pm1b_cnt_blk, (uint32_t)(slp_a | SLP_EN), (uint32_t)(slp_b | SLP_EN));

  /* Resumed: re-establish chipset timers/PIC with interrupts masked, then let the restored IF take over. */
  __asm__ volatile("cli");
  irq_resume(pic_mask1, pic_mask2); /* re-map PIC, restore masks */
  timer_init();                     /* re-program the PIT tick   */
  __asm__ volatile("sti");
  backlight_resume();
}

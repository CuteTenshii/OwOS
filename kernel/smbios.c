/* smbios.c — best-effort SMBIOS/DMI reader (vendor + product for neofetch).
 *
 * Three ways to reach the tables, tried in order of reliability:
 *   1. UEFI: walk the EFI system table's configuration-table array for the
 *      SMBIOS GUID — the canonical UEFI route, and the only one that survives
 *      ExitBootServices (the entry point + tables sit in persistent firmware
 *      memory). This is what real laptops and VMware/OVMF need.
 *   2. The multiboot2 SMBIOS tag's copied entry point (if GRUB supplied one).
 *   3. Legacy BIOS: scan the f-segment (0xF0000..0xFFFFF) for the anchor.
 * Everything is defensive: each firmware region is identity-mapped before it's
 * read (so a stray address can't fault), counts are bounded, and any failure
 * records smbios_error() and falls through rather than crashing.
 */

#include "smbios.h"
#include "paging.h"

/* Entry-point physical address copied from the multiboot2 SMBIOS tag, the EFI
 * system-table pointer from the EFI tag, and which width that pointer is. */
static uint32_t g_entry_phys;
static uint64_t g_efi_systab;
static int g_efi_is64;

/* Strings copied out of DMI type 1 while the tables are reachable. */
static char vendor[64];
static char product[64];
static char host[130];
static char errbuf[64]; /* why the last attempt found nothing             */
static int parsed;      /* smbios_init() ran (success or not)             */

void smbios_set_entry(uint32_t phys) { g_entry_phys = phys; }

void smbios_set_efi(uint64_t systab, int is64)
{
  g_efi_systab = systab;
  g_efi_is64 = is64;
}

/* Copy at most cap-1 bytes of a NUL-terminated string, NUL-terminating dst. */
static void copy_str(char *dst, uint32_t cap, const char *src)
{
  uint32_t i = 0;
  if (cap == 0)
    return;
  for (; src && src[i] && i < cap - 1; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static void set_err(const char *s) { copy_str(errbuf, sizeof errbuf, s); }

static bool streq4(const uint8_t *p, char a, char b, char c, char d)
{
  return p[0] == a && p[1] == b && p[2] == c && p[3] == d;
}

static bool bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return false;
  return true;
}

/* The two SMBIOS configuration-table GUIDs, in EFI's mixed-endian byte layout
 * (data1/2/3 little-endian, data4 as-is). 2.x ("_SM_") and 3.x ("_SM3_"). */
static const uint8_t SMBIOS_GUID[16] = {0x31, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
                                        0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d};
static const uint8_t SMBIOS3_GUID[16] = {0x44, 0x15, 0xfd, 0xf2, 0x94, 0x97, 0x2c, 0x4a,
                                         0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94};

/* Return the n-th (1-based) string from a structure's trailing string set, or
 * "" when n is 0 or runs past the set. `s` points at the first byte after the
 * formatted area; the set is NUL-terminated strings ended by a double NUL. */
static const char *dmi_string(const char *s, uint8_t n)
{
  if (n == 0)
    return "";
  while (--n) {
    while (*s)
      s++;
    s++; /* step over the terminator               */
    if (*s == 0)
      return ""; /* hit the double NUL: n was too large    */
  }
  return s;
}

/* Map `len` bytes at physical `phys` so we can read them, returning a pointer.
 * Low memory (the f-segment, where BIOS puts the tables) is already in the
 * identity map; firmware regions past 16 MiB need an explicit mapping. Mapping
 * first means a stray address yields garbage, never a page fault. */
static const uint8_t *map_phys(uint32_t phys, uint32_t len)
{
  if (phys >= 0x1000000)
    paging_identity_map(phys, len);
  return (const uint8_t *)(uintptr_t)phys;
}

/* Walk the structure table at `addr` (length `len`) for the first DMI type 1
 * record and copy its Manufacturer + Product Name out. Returns 1 if found. */
static int parse_structures(uint32_t addr, uint32_t len)
{
  if (!addr || !len || len > 0x100000) {
    set_err("bad struct table");
    return 0;
  }
  const uint8_t *base = map_phys(addr, len);
  const uint8_t *p = base;
  const uint8_t *end = base + len;
  while (p + 4 <= end) {
    uint8_t type = p[0];
    uint8_t flen = p[1]; /* formatted-area length                  */
    if (flen < 4)
      break; /* malformed; bail rather than loop       */
    const char *strset = (const char *)(p + flen);
    if (type == 1) { /* System Information                     */
      /* offset 4 = Manufacturer, offset 5 = Product Name (str indices) */
      copy_str(vendor, sizeof vendor, dmi_string(strset, p[4]));
      copy_str(product, sizeof product, dmi_string(strset, p[5]));
      return 1;
    }
    if (type == 127) /* end-of-table marker                    */
      break;
    /* Skip the formatted area, then the string set (ends in double NUL).
     * An empty string set is a single extra NUL after the formatted area. */
    p = (const uint8_t *)strset;
    while (p + 1 < end && (p[0] || p[1]))
      p++;
    p += 2;
  }
  set_err("no DMI type 1");
  return 0;
}

/* Validate and decode an entry-point structure at `ep`, dispatching to the
 * structure-table walk. Handles both the 32-bit "_SM_" (SMBIOS 2.x) and the
 * 64-bit "_SM3_" (SMBIOS 3.x) anchors. Returns 1 if a table was located. */
static int parse_entry(const uint8_t *ep)
{
  if (streq4(ep, '_', 'S', 'M', '3') && ep[4] == '_') {
    /* 3.x: 8-byte table address at 0x10, 4-byte max size at 0x0C. We only
     * address 32 bits — if the high half is set the table is unreachable. */
    uint32_t hi = *(const uint32_t *)(ep + 0x14);
    if (hi) {
      set_err("SMBIOS3 table >4GB");
      return 0;
    }
    uint32_t addr = *(const uint32_t *)(ep + 0x10);
    uint32_t max = *(const uint32_t *)(ep + 0x0C);
    return parse_structures(addr, max);
  }
  if (streq4(ep, '_', 'S', 'M', '_')) {
    /* 2.x: 2-byte table length at 0x16, 4-byte table address at 0x18. */
    uint32_t len = *(const uint16_t *)(ep + 0x16);
    uint32_t addr = *(const uint32_t *)(ep + 0x18);
    return parse_structures(addr, len);
  }
  set_err("no SMBIOS anchor");
  return 0;
}

/* UEFI path: walk the EFI system table's configuration tables for the SMBIOS
 * GUID and parse the entry point it points at. The EFI table layout differs
 * between 32- and 64-bit firmware (pointer width), so the field offsets and the
 * config-entry stride are chosen from g_efi_is64. OVMF and real UEFI keep all
 * of this in the low 4 GiB, so 64-bit pointers truncate cleanly; we bail if one
 * ever exceeds 4 GiB rather than read the wrong address. */
static int from_efi(void)
{
  if (!g_efi_systab)
    return 0;
  if (g_efi_systab >> 32) {
    set_err("EFI systab >4GB");
    return 0;
  }
  uint32_t st = (uint32_t)g_efi_systab;
  const uint8_t *t = map_phys(st, 128);

  /* NumberOfTableEntries (UINTN) then ConfigurationTable (ptr) sit right
   * after the 10 header/handle/protocol slots; their width is the pointer
   * width. 64-bit: count @104, array @112. 32-bit: count @64, array @68. */
  uint32_t count, arr;
  uint32_t stride;
  if (g_efi_is64) {
    count = *(const uint32_t *)(t + 104); /* low half of UINTN        */
    uint64_t a = *(const uint64_t *)(t + 112);
    if (a >> 32) {
      set_err("EFI cfgtab >4GB");
      return 0;
    }
    arr = (uint32_t)a;
    stride = 24; /* 16-byte GUID + 8-byte ptr */
  } else {
    count = *(const uint32_t *)(t + 64);
    arr = *(const uint32_t *)(t + 68);
    stride = 20; /* 16-byte GUID + 4-byte ptr */
  }
  if (count == 0 || count > 1024 || !arr) {
    set_err("EFI cfgtab empty");
    return 0;
  }

  const uint8_t *ct = map_phys(arr, count * stride);
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *e = ct + (uint32_t)i * stride;
    if (!bytes_eq(e, SMBIOS_GUID, 16) && !bytes_eq(e, SMBIOS3_GUID, 16))
      continue;
    uint64_t vt = g_efi_is64 ? *(const uint64_t *)(e + 16) : (uint64_t)*(const uint32_t *)(e + 16);
    if (vt >> 32) {
      set_err("SMBIOS EP >4GB");
      continue;
    }
    const uint8_t *ep = map_phys((uint32_t)vt, 0x20);
    if (parse_entry(ep))
      return 1;
  }
  set_err("no SMBIOS in EFI cfgtab");
  return 0;
}

/* True when `product` already begins with `vendor` (case-insensitive, e.g.
 * vendor "HP" + product "HP EliteBook 840 G3") so the join shouldn't repeat it.
 */
static bool product_has_vendor_prefix(void)
{
  for (int i = 0; vendor[i]; i++) {
    char a = vendor[i], b = product[i];
    if (a >= 'A' && a <= 'Z')
      a += 32;
    if (b >= 'A' && b <= 'Z')
      b += 32;
    if (a != b)
      return false;
  }
  return true;
}

void smbios_init(void)
{
  if (parsed)
    return;
  parsed = 1;
  set_err("not probed");

  /* UEFI: the configuration-table walk is the reliable route. */
  if (from_efi())
    goto build;

  /* mb2 SMBIOS tag: a copied entry point (BIOS GRUB, or a UEFI GRUB that
   * still supplies it). */
  if (g_entry_phys) {
    const uint8_t *ep = map_phys(g_entry_phys, 0x20);
    if (parse_entry(ep))
      goto build;
  }

  /* Legacy BIOS: the anchor is on a 16-byte boundary in the f-segment. */
  for (uint32_t a = 0xF0000; a < 0x100000; a += 16) {
    const uint8_t *ep = (const uint8_t *)(uintptr_t)a;
    if (streq4(ep, '_', 'S', 'M', '_') || (streq4(ep, '_', 'S', 'M', '3') && ep[4] == '_')) {
      if (parse_entry(ep))
        goto build;
    }
  }

build:
  /* Compose "<vendor> <product>", skipping whichever half is missing. Some
   * firmwares fill one field with filler like "To Be Filled By O.E.M." — we
   * print it verbatim rather than trying to second-guess every vendor.
   * Many OEMs (HP, Dell, LENOVO) already repeat the vendor at the start of the
   * product string, so a naive join reads "HP HP EliteBook 840 G3" — drop the
   * redundant vendor prefix (case-insensitive) when the product carries it. */
  if (vendor[0] && product[0] && !product_has_vendor_prefix()) {
    copy_str(host, sizeof host, vendor);
    uint32_t n = 0;
    while (host[n])
      n++;
    if (n + 1 < sizeof host) {
      host[n++] = ' ';
      copy_str(host + n, sizeof host - n, product);
    }
  } else if (product[0]) {
    copy_str(host, sizeof host, product);
  } else {
    copy_str(host, sizeof host, vendor); /* vendor-only, or "" */
  }
  if (host[0])
    set_err("");
}

const char *smbios_vendor(void) { return vendor; }
const char *smbios_product(void) { return product; }
const char *smbios_host(void) { return host; }
const char *smbios_error(void) { return errbuf; }

/* cpu.c: the shared cpu.h API from the AArch64 ID registers. */
#include "cpu.h"
#include "arch.h"
#include "kstring.h"

struct named {
  uint32_t id;
  const char *name;
};

static const struct named implementers[] = {
    {0x41, "ARM"},      {0x42, "Broadcom"}, {0x43, "Cavium"}, {0x4E, "NVIDIA"},
    {0x51, "Qualcomm"}, {0x53, "Samsung"},  {0x61, "Apple"},  {0xC0, "Ampere"},
};

/* ARM's own part numbers, the cores the supported boards and QEMU use. */
static const struct named arm_parts[] = {
    {0xD03, "Cortex-A53"},  {0xD04, "Cortex-A35"}, {0xD05, "Cortex-A55"}, {0xD07, "Cortex-A57"},
    {0xD08, "Cortex-A72"},  {0xD09, "Cortex-A73"}, {0xD0A, "Cortex-A75"}, {0xD0B, "Cortex-A76"},
    {0xD0C, "Neoverse-N1"}, {0xD0D, "Cortex-A77"}, {0xD41, "Cortex-A78"}, {0xD44, "Cortex-X1"},
};

static const char *lookup(const struct named *t, unsigned n, uint32_t id)
{
  for (unsigned i = 0; i < n; i++)
    if (t[i].id == id)
      return t[i].name;
  return 0;
}

static uint32_t midr_implementer(uint64_t midr) { return (uint32_t)(midr >> 24) & 0xFF; }
static uint32_t midr_part(uint64_t midr) { return (uint32_t)(midr >> 4) & 0xFFF; }

void cpu_vendor(char *buf13)
{
  uint64_t midr = SYSREG_READ(midr_el1);
  const char *name = lookup(implementers, sizeof implementers / sizeof *implementers, midr_implementer(midr));
  if (name)
    strlcpy(buf13, name, 13);
  else
    ksnprintf(buf13, 13, "impl 0x%x", midr_implementer(midr));
}

void cpu_brand(char *buf49)
{
  uint64_t midr = SYSREG_READ(midr_el1);
  char vendor[13];
  cpu_vendor(vendor);
  const char *part =
      midr_implementer(midr) == 0x41 ? lookup(arm_parts, sizeof arm_parts / sizeof *arm_parts, midr_part(midr)) : 0;
  unsigned variant = (unsigned)(midr >> 20) & 0xF, revision = (unsigned)midr & 0xF;
  if (part)
    ksnprintf(buf49, 49, "%s %s r%up%u", vendor, part, variant, revision);
  else
    ksnprintf(buf49, 49, "%s part 0x%x r%up%u", vendor, midr_part(midr), variant, revision);
}

void cpu_signature(struct cpu_signature *sig)
{
  uint64_t midr = SYSREG_READ(midr_el1);
  sig->family = (uint32_t)(midr >> 16) & 0xF; /* architecture */
  sig->model = midr_part(midr);
  sig->stepping = (uint32_t)midr & 0xF;
}

void cpu_address_bits(uint32_t *phys, uint32_t *virt)
{
  static const uint32_t pa_bits[] = {32, 36, 40, 42, 44, 48, 52};
  uint64_t parange = SYSREG_READ(id_aa64mmfr0_el1) & 0xF;
  *phys = parange < sizeof pa_bits / sizeof *pa_bits ? pa_bits[parange] : 48;
  *virt = 64 - (uint32_t)(SYSREG_READ(tcr_el1) & 0x3F); /* the kernel's TTBR0 range, 64 - T0SZ */
}

struct feature {
  int isar0;      /* ID_AA64ISAR0_EL1, else ID_AA64PFR0_EL1 */
  unsigned shift; /* the 4-bit field */
  unsigned min;
  const char *name;
};

static const struct feature features[] = {
    {0, 16, 0, "fp"}, /* PFR0.FP: 0 means implemented, 0xF absent */
    {0, 20, 0, "asimd"}, {1, 4, 1, "aes"},    {1, 4, 2, "pmull"},    {1, 8, 1, "sha1"},
    {1, 12, 1, "sha2"},  {1, 16, 1, "crc32"}, {1, 20, 2, "atomics"}, {1, 60, 1, "rng"},
};

void cpu_features(char *buf, uint32_t size)
{
  if (size == 0)
    return;
  uint64_t isar0 = SYSREG_READ(id_aa64isar0_el1), pfr0 = SYSREG_READ(id_aa64pfr0_el1);
  uint32_t pos = 0;
  for (unsigned i = 0; i < sizeof features / sizeof *features; i++) {
    const struct feature *f = &features[i];
    unsigned field = (unsigned)((f->isar0 ? isar0 : pfr0) >> f->shift) & 0xF;
    if (f->isar0 ? field < f->min : field == 0xF)
      continue;
    if (pos != 0 && pos + 1 < size)
      buf[pos++] = ' ';
    for (const char *p = f->name; *p && pos + 1 < size; p++)
      buf[pos++] = *p;
  }
  buf[pos] = '\0';
}

/* The core clock is not architecturally visible (the generic timer runs off a separate reference), so 0: unknown. */
uint32_t cpu_mhz(void) { return 0; }

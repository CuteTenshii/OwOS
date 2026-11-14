/* cpu.c: the shared cpu.h API from the AArch64 ID registers and the PMU cycle counter. */
#include "cpu.h"
#include "arch.h"
#include "irqflags.h"
#include "kstring.h"
#include "timer.h"

#define PMCR_E (1u << 0)  /* counters enabled */
#define PMCR_D (1u << 3)  /* cycle counter divides by 64 */
#define PMCR_LC (1u << 6) /* 64-bit cycle counter */
#define PMCNTEN_C (1u << 31)
#define MHZ_WINDOW_US 20000

/* Big and LITTLE cores report different MIDRs, so each CPU records its own as it comes up. */
static uint64_t cpu_midr[MAX_CPUS];

void cpu_record(int cpu)
{
  if (cpu >= 0 && cpu < MAX_CPUS)
    cpu_midr[cpu] = SYSREG_READ(midr_el1);
}

struct named {
  uint32_t id;
  const char *name;
};

static const struct named implementers[] = {
    {0x41, "ARM"},    {0x42, "Broadcom"}, {0x43, "Cavium"},    {0x46, "Fujitsu"}, {0x48, "HiSilicon"},
    {0x4E, "NVIDIA"}, {0x50, "APM"},      {0x51, "Qualcomm"},  {0x53, "Samsung"}, {0x56, "Marvell"},
    {0x61, "Apple"},  {0x69, "Intel"},    {0x6D, "Microsoft"}, {0xC0, "Ampere"},
};

struct part {
  uint8_t implementer;
  uint16_t part;
  const char *name;
};

static const struct part parts[] = {
    {0x41, 0xD01, "Cortex-A32"},       {0x41, 0xD02, "Cortex-A34"},       {0x41, 0xD03, "Cortex-A53"},
    {0x41, 0xD04, "Cortex-A35"},       {0x41, 0xD05, "Cortex-A55"},       {0x41, 0xD06, "Cortex-A65"},
    {0x41, 0xD07, "Cortex-A57"},       {0x41, 0xD08, "Cortex-A72"},       {0x41, 0xD09, "Cortex-A73"},
    {0x41, 0xD0A, "Cortex-A75"},       {0x41, 0xD0B, "Cortex-A76"},       {0x41, 0xD0C, "Neoverse-N1"},
    {0x41, 0xD0D, "Cortex-A77"},       {0x41, 0xD0E, "Cortex-A76AE"},     {0x41, 0xD40, "Neoverse-V1"},
    {0x41, 0xD41, "Cortex-A78"},       {0x41, 0xD42, "Cortex-A78AE"},     {0x41, 0xD43, "Cortex-A65AE"},
    {0x41, 0xD44, "Cortex-X1"},        {0x41, 0xD46, "Cortex-A510"},      {0x41, 0xD47, "Cortex-A710"},
    {0x41, 0xD48, "Cortex-X2"},        {0x41, 0xD49, "Neoverse-N2"},      {0x41, 0xD4A, "Neoverse-E1"},
    {0x41, 0xD4B, "Cortex-A78C"},      {0x41, 0xD4C, "Cortex-X1C"},       {0x41, 0xD4D, "Cortex-A715"},
    {0x41, 0xD4E, "Cortex-X3"},        {0x41, 0xD4F, "Neoverse-V2"},      {0x41, 0xD80, "Cortex-A520"},
    {0x41, 0xD81, "Cortex-A720"},      {0x41, 0xD82, "Cortex-X4"},        {0x41, 0xD84, "Neoverse-V3"},
    {0x41, 0xD85, "Cortex-X925"},      {0x41, 0xD87, "Cortex-A725"},      {0x41, 0xD8E, "Neoverse-N3"},
    {0x43, 0x0A1, "ThunderX"},         {0x43, 0x0AF, "ThunderX2"},        {0x46, 0x001, "A64FX"},
    {0x48, 0xD01, "TaiShan-v110"},     {0x4E, 0x000, "Denver"},           {0x4E, 0x003, "Denver 2"},
    {0x4E, 0x004, "Carmel"},           {0x51, 0x800, "Kryo 2xx Gold"},    {0x51, 0x801, "Kryo 2xx Silver"},
    {0x51, 0x802, "Kryo 3xx Gold"},    {0x51, 0x803, "Kryo 3xx Silver"},  {0x51, 0x804, "Kryo 4xx Gold"},
    {0x51, 0x805, "Kryo 4xx Silver"},  {0x51, 0xC00, "Falkor"},           {0x51, 0xC01, "Saphira"},
    {0x53, 0x001, "Exynos-M1"},        {0x53, 0x002, "Exynos-M3"},        {0x53, 0x003, "Exynos-M4"},
    {0x53, 0x004, "Exynos-M5"},        {0x61, 0x022, "M1 Icestorm"},      {0x61, 0x023, "M1 Firestorm"},
    {0x61, 0x024, "M1 Pro Icestorm"},  {0x61, 0x025, "M1 Pro Firestorm"}, {0x61, 0x028, "M1 Max Icestorm"},
    {0x61, 0x029, "M1 Max Firestorm"}, {0x61, 0x032, "M2 Blizzard"},      {0x61, 0x033, "M2 Avalanche"},
    {0xC0, 0xAC3, "Ampere-1"},         {0xC0, 0xAC4, "Ampere-1a"},
};

static uint32_t midr_implementer(uint64_t midr) { return (uint32_t)(midr >> 24) & 0xFF; }
static uint32_t midr_part(uint64_t midr) { return (uint32_t)(midr >> 4) & 0xFFF; }
/* Implementer and part: the core design, without its variant and revision. */
static uint64_t midr_core(uint64_t midr) { return midr & 0xFF00FFF0ull; }

static const char *part_name(uint64_t midr)
{
  for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++)
    if (parts[i].implementer == midr_implementer(midr) && parts[i].part == midr_part(midr))
      return parts[i].name;
  return 0;
}

static void vendor_of(uint64_t midr, char *buf13)
{
  for (unsigned i = 0; i < sizeof implementers / sizeof *implementers; i++)
    if (implementers[i].id == midr_implementer(midr)) {
      strlcpy(buf13, implementers[i].name, 13);
      return;
    }
  ksnprintf(buf13, 13, "impl 0x%x", midr_implementer(midr));
}

/* The boot CPU's MIDR: a caller may ask before cpu_record ran. */
static uint64_t boot_midr(void) { return cpu_midr[0] ? cpu_midr[0] : SYSREG_READ(midr_el1); }

void cpu_vendor(char *buf13) { vendor_of(boot_midr(), buf13); }

/* "ARM Cortex-A57 r1p0" when every core is the same design, else each design with its count, biggest cluster first
 * in boot order: "ARM 4x Cortex-A76 + 4x Cortex-A55". */
void cpu_brand(char *buf49)
{
  uint64_t kinds[MAX_CPUS];
  int counts[MAX_CPUS], nkinds = 0;
  for (int c = 0; c < MAX_CPUS; c++) {
    uint64_t midr = c == 0 ? boot_midr() : cpu_midr[c];
    if (!midr)
      continue;
    int k = 0;
    while (k < nkinds && kinds[k] != midr_core(midr))
      k++;
    if (k == nkinds) {
      kinds[nkinds] = midr_core(midr);
      counts[nkinds++] = 0;
    }
    counts[k]++;
  }

  uint64_t midr = boot_midr();
  char vendor[13];
  cpu_vendor(vendor);
  if (nkinds <= 1) {
    const char *part = part_name(midr);
    unsigned variant = (unsigned)(midr >> 20) & 0xF, revision = (unsigned)midr & 0xF;
    if (part)
      ksnprintf(buf49, 49, "%s %s r%up%u", vendor, part, variant, revision);
    else
      ksnprintf(buf49, 49, "%s part 0x%x r%up%u", vendor, midr_part(midr), variant, revision);
    return;
  }

  /* A stable sort by count keeps boot order among equal clusters. */
  for (int i = 1; i < nkinds; i++)
    for (int j = i; j > 0 && counts[j] > counts[j - 1]; j--) {
      uint64_t tk = kinds[j];
      kinds[j] = kinds[j - 1];
      kinds[j - 1] = tk;
      int tc = counts[j];
      counts[j] = counts[j - 1];
      counts[j - 1] = tc;
    }
  bool one_vendor = true;
  for (int k = 1; k < nkinds; k++)
    one_vendor &= midr_implementer(kinds[k]) == midr_implementer(kinds[0]);
  uint32_t pos = 0;
  buf49[0] = '\0';
  if (one_vendor) {
    vendor_of(kinds[0], vendor);
    ksnprintf(buf49, 49, "%s ", vendor);
    pos = (uint32_t)strlen(buf49);
  }
  for (int k = 0; k < nkinds && pos < 48; k++) {
    char name[24];
    const char *part = part_name(kinds[k]);
    if (part)
      strlcpy(name, part, sizeof name);
    else
      ksnprintf(name, sizeof name, "part 0x%x", midr_part(kinds[k]));
    ksnprintf(buf49 + pos, 49 - pos, "%s%dx %s", k ? " + " : "", counts[k], name);
    pos = (uint32_t)strlen(buf49);
  }
}

void cpu_signature(struct cpu_signature *sig)
{
  uint64_t midr = boot_midr();
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

static bool pmu_present(void)
{
  unsigned ver = (unsigned)(SYSREG_READ(id_aa64dfr0_el1) >> 8) & 0xF;
  return ver != 0 && ver != 0xF;
}

/* The core clock is not architecturally visible, but the PMU cycle counter runs on it: count cycles over a window of
 * the generic timer. Interrupts stay off so the task can't migrate to a core with another counter. 0 without a PMU. */
uint32_t cpu_mhz(void)
{
  if (!pmu_present())
    return 0;
  uintptr_t flags = irq_save();
  uint64_t pmcr = SYSREG_READ(pmcr_el0), enabled = SYSREG_READ(pmcntenset_el0), filter = SYSREG_READ(pmccfiltr_el0);
  SYSREG_WRITE(pmccfiltr_el0, 0); /* count at EL0 and EL1 alike */
  SYSREG_WRITE(pmcr_el0, (pmcr | PMCR_E | PMCR_LC) & ~(uint64_t)PMCR_D);
  SYSREG_WRITE(pmcntenset_el0, PMCNTEN_C);
  __asm__ volatile("isb");

  uint64_t t0 = timer_us64(), c0 = SYSREG_READ(pmccntr_el0), t1;
  while ((t1 = timer_us64()) - t0 < MHZ_WINDOW_US)
    cpu_relax();
  uint64_t cycles = SYSREG_READ(pmccntr_el0) - c0, us = t1 - t0;

  if (!(enabled & PMCNTEN_C))
    SYSREG_WRITE(pmcntenclr_el0, PMCNTEN_C);
  SYSREG_WRITE(pmccfiltr_el0, filter);
  SYSREG_WRITE(pmcr_el0, pmcr);
  irq_restore(flags);
  return (uint32_t)((cycles + us / 2) / us);
}

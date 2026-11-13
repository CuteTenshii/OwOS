/* bootmod.c: multiboot modules (see bootmod.h). */
#include "bootmod.h"
#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"

#define MAX_MODULES 4
#define CMDLINE_LEN 64

static struct {
  uint32_t start, len;
  char cmdline[CMDLINE_LEN];
} mods[MAX_MODULES];
static int nmods;

void bootmod_add(uint32_t start, uint32_t end, const char *cmdline)
{
  if (nmods == MAX_MODULES || end <= start)
    return;
  strlcpy(mods[nmods].cmdline, cmdline ? cmdline : "", CMDLINE_LEN);
  mods[nmods].start = start;
  mods[nmods].len = end - start;
  nmods++;
}

void bootmod_reserve(void)
{
  for (int i = 0; i < nmods; i++)
    pmm_reserve_range(mods[i].start, mods[i].len);
}

void bootmod_relocate(void)
{
  for (int i = 0; i < nmods; i++) {
    uint32_t len = mods[i].len;
    bool kept;
    if (mods[i].start < pmm_low_end()) {
      /* A module can start below the low identity map and run past its end. */
      phys_t to = pmm_alloc_contig(len);
      kept = to && paging_identity_map(mods[i].start, len) == 0 && paging_identity_map((uint32_t)to, len) == 0;
      if (kept) {
        memcpy((void *)(uintptr_t)to, (const void *)(uintptr_t)mods[i].start, len);
        mods[i].start = (uint32_t)to;
      }
    } else {
      kept = paging_identity_map(mods[i].start, len) == 0;
    }
    con_printf(kept ? "bootmod: \"%s\", %u KiB\n" : "bootmod: no room to keep \"%s\" (%u KiB)\n", mods[i].cmdline,
               len / 1024);
    if (!kept)
      mods[i].len = 0;
  }
}

/* Whether `name` is one of the space-separated words of `cmdline`. */
static bool has_word(const char *cmdline, const char *name)
{
  uint32_t n = strlen(name);
  for (const char *p = cmdline; *p; p++)
    if ((p == cmdline || p[-1] == ' ') && memcmp(p, name, n) == 0 && (p[n] == ' ' || p[n] == '\0'))
      return true;
  return false;
}

const void *bootmod_find(const char *name, uint32_t *len)
{
  for (int i = 0; i < nmods; i++)
    if (mods[i].len && has_word(mods[i].cmdline, name)) {
      *len = mods[i].len;
      return (const void *)(uintptr_t)mods[i].start;
    }
  return 0;
}

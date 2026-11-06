/* lspci.c, a PCI device lister */

#include "owos.h"

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  static char buf[8192];
  int n = readfile("/proc/pci", buf, (int)sizeof buf - 1);
  if (n <= 0) {
    puts("no PCI devices found\n");
    return 0;
  }
  puts("bus:slot.fn vendor:device class description\n");
  write(1, buf, n);
  return 0;
}

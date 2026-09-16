/* lspci.c — a PCI device lister.
 *
 * Like its Linux namesake (which reads from sysfs, not by living in the
 * kernel), this is pure presentation: the kernel scanned the bus at boot and
 * exposes the list as the ordinary file /proc/pci. lspci just reads and prints
 * it — no bespoke syscall, and a bug here is a contained crash, not a panic.
 *
 *   lspci
 */

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

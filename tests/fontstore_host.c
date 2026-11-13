/* Host stand-in for user/lib/font/fontstore.c, whose /etc/fonts scan needs the guest readdir: no font is installed,
 * so code that loads the default faces gets none, and tests hand it fonts directly. */
#include "fontstore.h"
#include <stdio.h>

void fontstore_path(int role, char *path, int n)
{
  (void)role;
  snprintf(path, (size_t)n, ".");
}

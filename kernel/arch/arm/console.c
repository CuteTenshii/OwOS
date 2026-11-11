/* console.c: route the generic kernel's console output to the PL011 UART. */
#include "console.h"
#include "arch.h"

void con_write(const char *s) { uart_puts(s); }

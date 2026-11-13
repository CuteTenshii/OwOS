/* diskinstall.h: SYS_INSTALL, the kernel side of the Installer app. Shared by the kernel and user space. */
#ifndef OWOS_COMMON_DISKINSTALL_H
#define OWOS_COMMON_DISKINSTALL_H

#include <stdint.h>

/* SYS_INSTALL operations, in ebx; root or a valid sudo only. */
#define INSTALL_LIST 0  /* ecx = index, edx = struct install_disk *: 1 filled, 0 past the end, -1 on error */
#define INSTALL_START 1 /* ecx = device name ("ide0"): 0, or the negative install_problem refusing it */
#define INSTALL_STEP 2  /* ecx = struct install_progress *: do the next chunk; 0 running, 1 finished, -1 failed */

/* Why a disk cannot take OwOS. */
enum install_problem {
  INSTALL_OK = 0,
  INSTALL_TOO_SMALL = 1,   /* below INSTALL_MIN_SECTORS */
  INSTALL_READ_ONLY = 2,   /* a CD, or a device without write support */
  INSTALL_LIVE_MEDIUM = 3, /* holds an ISO9660 volume: probably the medium OwOS is running from */
  INSTALL_NO_IMAGE = 4,    /* this boot carries no kernel image or GRUB images to copy */
  INSTALL_BUSY = 5,        /* an install is already running */
  INSTALL_NO_DEVICE = 6,
  INSTALL_DENIED = 7,
};

/* 1 MiB BIOS boot partition from 1 MiB, 128 MiB ESP after it, then at least 128 MiB of OwOS root. */
#define INSTALL_MIN_SECTORS (266240u + 262144u + 33u)

struct install_disk {
  char dev[16];
  char model[40];
  uint32_t sectors; /* 512-byte sectors */
  int32_t problem;  /* enum install_problem */
};

struct install_progress {
  uint32_t done, total; /* work units */
  char stage[40];       /* what is being done, or what failed, for display */
};

#endif /* OWOS_COMMON_DISKINSTALL_H */

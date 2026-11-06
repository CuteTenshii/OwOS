/* timeval64.h: wall-clock time as exchanged by SYS_GETTIME/SETTIME/ADJTIME (user and kernel). */
#ifndef OWOS_TIMEVAL64_H
#define OWOS_TIMEVAL64_H

/* tv_usec is always in [0, 1000000); negative times floor tv_sec. */
struct timeval64 {
  long long tv_sec;
  int tv_usec;
} __attribute__((packed, aligned(4)));

#endif

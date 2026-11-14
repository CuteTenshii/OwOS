#include "syscall_net.h"
#include "entropy.h"
#include "heap.h"
#include "net.h"
#include "regs.h"
#include "rtc.h"
#include "sched.h"
#include "syscall_path.h"
#include "uaccess.h"
#include "wallclock.h"
#include <stdbool.h>
#include <stdint.h>

void syscall_net_connect(struct registers *r)
{
  if ((int)SC_ARG2(r) == SK_RAWETH) {
    SC_RET(r) = sched_uid() != 0 ? (uint32_t)-1 : (uint32_t)net_sock_connect(0, 0, SK_RAWETH);
    return;
  }
  char host[NET_HOST_MAX];
  if (copyin_str(SC_ARG0(r), host, sizeof host) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)net_sock_connect(host, (uint16_t)SC_ARG1(r), (int)SC_ARG2(r));
}

/* Socket calls can block (ARP, TCP window), and a sibling thread may unmap the user buffer meanwhile, so data moves
 * through a kernel buffer. A chunk is above every datagram limit, so an oversized datagram still fails whole. */
#define NET_CHUNK 65536u

void syscall_net_send(struct registers *r)
{
  sys_word a[6];
  if (!user_range(SC_ARG0(r), sizeof a) || copyin(a, SC_ARG0(r), sizeof a) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint32_t fd = (uint32_t)a[0], len = (uint32_t)a[2], port = (uint32_t)a[5];
  uaddr_t buf = a[1], ip_ptr = a[4];
  if (len == 0 || !user_range(buf, len) || (ip_ptr && !user_range(ip_ptr, 16))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint8_t ip[16];
  if (ip_ptr && copyin(ip, ip_ptr, sizeof ip) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint32_t cap = len < NET_CHUNK ? len : NET_CHUNK;
  uint8_t *kbuf = kmalloc(cap);
  if (!kbuf) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint32_t done = 0;
  int err = 0; /* first failure, reported only when nothing was sent (-4 = interrupted) */
  while (done < len) {
    uint32_t n = len - done < cap ? len - done : cap;
    if (copyin(kbuf, buf + done, n) != 0) {
      err = -1;
      break;
    }
    int w = ip_ptr ? net_sock_sendto((int)fd, ip, (int)port, kbuf, n) : net_sock_send((int)fd, kbuf, n);
    if (w < 0) {
      err = w;
      break;
    }
    done += (uint32_t)w;
    if ((uint32_t)w < n)
      break;
  }
  kfree(kbuf);
  SC_RET(r) = done || !err ? done : (uint32_t)err;
}

void syscall_net_recv(struct registers *r)
{
  sys_word a[6];
  if (!user_range(SC_ARG0(r), sizeof a) || copyin(a, SC_ARG0(r), sizeof a) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint32_t fd = (uint32_t)a[0], max = (uint32_t)a[2], flags = (uint32_t)a[3];
  uaddr_t buf = a[1], ip_ptr = a[4], port_ptr = a[5];
  if (max == 0 || !user_out_range(buf, max) || (ip_ptr && !user_out_range(ip_ptr, 16)) ||
      (port_ptr && !user_out_range(port_ptr, 4))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (max > NET_CHUNK)
    max = NET_CHUNK;
  uint8_t *kbuf = kmalloc(max);
  if (!kbuf) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  int n;
  if (ip_ptr || port_ptr) {
    uint8_t ip[16];
    uint16_t port = 0;
    n = net_sock_recvfrom((int)fd, kbuf, max, ip_ptr ? ip : 0, &port);
    if (n > 0) {
      if (ip_ptr && copyout(ip_ptr, ip, sizeof ip) != 0)
        n = -1;
      if (port_ptr && n > 0) {
        int port_out = port;
        if (copyout(port_ptr, &port_out, sizeof port_out) != 0)
          n = -1;
      }
    }
  } else if (flags & MSG_DONTWAIT) {
    n = net_sock_recv_nb((int)fd, kbuf, max);
  } else {
    n = net_sock_recv((int)fd, kbuf, max);
  }
  if (n > 0 && copyout(buf, kbuf, (uint32_t)n) != 0)
    n = -1;
  kfree(kbuf);
  SC_RET(r) = (uint32_t)n;
}

void syscall_net_close(struct registers *r) { SC_RET(r) = (uint32_t)net_sock_close((int)SC_ARG0(r)); }

void syscall_net_setopt(struct registers *r)
{
  SC_RET(r) = (uint32_t)((int)SC_ARG1(r) == NET_OPT_TTL ? net_sock_set_ttl((int)SC_ARG0(r), (int)SC_ARG2(r)) : -1);
}

void syscall_net_ifconfig(struct registers *r)
{
  if (sched_uid() != 0 || !user_range(SC_ARG0(r), 4) || !user_range(SC_ARG1(r), 12)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint8_t ip[4], rest[12], mask[4], gw[4], dns[4];
  if (copyin(ip, SC_ARG0(r), sizeof ip) != 0 || copyin(rest, SC_ARG1(r), sizeof rest) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  for (int i = 0; i < 4; i++) {
    mask[i] = rest[i];
    gw[i] = rest[4 + i];
    dns[i] = rest[8 + i];
  }
  net_ifconfig(ip, mask, gw, dns);
  SC_RET(r) = 0;
}

void syscall_net_setrtc(struct registers *r)
{
  if (sched_uid() != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  struct timeval64 tv = {(uint32_t)SC_ARG0(r), 0};
  SC_RET(r) = wallclock_set(&tv) ? 0 : (uint32_t)-1;
}

void syscall_net_gettime(struct registers *r)
{
  struct timeval64 tv;
  wallclock_get(&tv);
  SC_RET(r) = copyout(SC_ARG0(r), &tv, sizeof tv) == 0 ? 0 : (uint32_t)-1;
}

void syscall_net_settime(struct registers *r)
{
  struct timeval64 tv;
  if (sched_uid() != 0 || copyin(&tv, SC_ARG0(r), sizeof tv) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = wallclock_set(&tv) ? 0 : (uint32_t)-1;
}

void syscall_net_adjtime(struct registers *r)
{
  struct timeval64 delta, old;
  bool has_delta = SC_ARG0(r) != 0;
  if (sched_uid() != 0 || (has_delta && copyin(&delta, SC_ARG0(r), sizeof delta) != 0) ||
      (SC_ARG1(r) && !user_out_range(SC_ARG1(r), sizeof old))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (!wallclock_adjust(has_delta ? &delta : 0, &old)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = SC_ARG1(r) && copyout(SC_ARG1(r), &old, sizeof old) != 0 ? (uint32_t)-1 : 0;
}

void syscall_net_listen(struct registers *r)
{
  if ((int)SC_ARG0(r) < 1024 && sched_uid() != 0)
    SC_RET(r) = (uint32_t)-1;
  else
    SC_RET(r) = (uint32_t)net_sock_listen((int)SC_ARG0(r));
}

void syscall_net_accept(struct registers *r) { SC_RET(r) = (uint32_t)net_sock_accept((int)SC_ARG0(r)); }

void syscall_net_bind(struct registers *r)
{
  if ((int)SC_ARG0(r) < 1024 && sched_uid() != 0)
    SC_RET(r) = (uint32_t)-1;
  else
    SC_RET(r) = (uint32_t)net_sock_bind((int)SC_ARG0(r));
}

#include "syscall_net.h"
#include "entropy.h"
#include "idt.h"
#include "net.h"
#include "rtc.h"
#include "sched.h"
#include "syscall_path.h"
#include "uaccess.h"
#include <stdint.h>

void syscall_net_connect(struct registers *r)
{
  if ((int)r->edx == SK_RAWETH) {
    r->eax = sched_uid() != 0 ? (uint32_t)-1 : (uint32_t)net_sock_connect(0, 0, SK_RAWETH);
    return;
  }
  char host[NET_HOST_MAX];
  if (copyin_str(r->ebx, host, sizeof host) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)net_sock_connect(host, (uint16_t)r->ecx, (int)r->edx);
}

void syscall_net_send(struct registers *r)
{
  uint32_t a[6];
  if (!user_range(r->ebx, sizeof a) || copyin(a, r->ebx, sizeof a) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint32_t fd = a[0], buf = a[1], len = a[2], ip_ptr = a[4], port = a[5];
  if (len == 0 || !user_range(buf, len) || (ip_ptr && !user_range(ip_ptr, 4))) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (ip_ptr) {
    uint8_t ip[4];
    if (copyin(ip, ip_ptr, sizeof ip) != 0) {
      r->eax = (uint32_t)-1;
      return;
    }
    r->eax = (uint32_t)net_sock_sendto((int)fd, ip, (int)port, (const void *)(uintptr_t)buf, len);
  } else {
    r->eax = (uint32_t)net_sock_send((int)fd, (const void *)(uintptr_t)buf, len);
  }
}

void syscall_net_recv(struct registers *r)
{
  uint32_t a[6];
  if (!user_range(r->ebx, sizeof a) || copyin(a, r->ebx, sizeof a) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint32_t fd = a[0], buf = a[1], max = a[2], flags = a[3];
  uint32_t ip_ptr = a[4], port_ptr = a[5];
  if (max == 0 || !user_out_range(buf, max) || (ip_ptr && !user_out_range(ip_ptr, 4)) ||
      (port_ptr && !user_out_range(port_ptr, 4))) {
    r->eax = (uint32_t)-1;
    return;
  }
  int n;
  if (ip_ptr || port_ptr) {
    uint8_t ip[4];
    uint16_t port = 0;
    n = net_sock_recvfrom((int)fd, (void *)(uintptr_t)buf, max, ip_ptr ? ip : 0, &port);
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
    n = net_sock_recv_nb((int)fd, (void *)(uintptr_t)buf, max);
  } else {
    n = net_sock_recv((int)fd, (void *)(uintptr_t)buf, max);
  }
  r->eax = (uint32_t)n;
}

void syscall_net_close(struct registers *r) { r->eax = (uint32_t)net_sock_close((int)r->ebx); }

void syscall_net_setopt(struct registers *r)
{
  r->eax = (uint32_t)((int)r->ecx == NET_OPT_TTL ? net_sock_set_ttl((int)r->ebx, (int)r->edx) : -1);
}

void syscall_net_ifconfig(struct registers *r)
{
  if (sched_uid() != 0 || !user_range(r->ebx, 4) || !user_range(r->ecx, 12)) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint8_t ip[4], rest[12], mask[4], gw[4], dns[4];
  if (copyin(ip, r->ebx, sizeof ip) != 0 || copyin(rest, r->ecx, sizeof rest) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  for (int i = 0; i < 4; i++) {
    mask[i] = rest[i];
    gw[i] = rest[4 + i];
    dns[i] = rest[8 + i];
  }
  net_ifconfig(ip, mask, gw, dns);
  r->eax = 0;
}

void syscall_net_setrtc(struct registers *r)
{
  if (sched_uid() != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct rtc_time t;
  epoch_to_rtc((uint32_t)r->ebx, &t);
  rtc_set(&t);
  r->eax = 0;
}

void syscall_net_listen(struct registers *r)
{
  if ((int)r->ebx < 1024 && sched_uid() != 0)
    r->eax = (uint32_t)-1;
  else
    r->eax = (uint32_t)net_sock_listen((int)r->ebx);
}

void syscall_net_accept(struct registers *r) { r->eax = (uint32_t)net_sock_accept((int)r->ebx); }

void syscall_net_bind(struct registers *r)
{
  if ((int)r->ebx < 1024 && sched_uid() != 0)
    r->eax = (uint32_t)-1;
  else
    r->eax = (uint32_t)net_sock_bind((int)r->ebx);
}

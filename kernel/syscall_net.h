#ifndef OWOS_SYSCALL_NET_H
#define OWOS_SYSCALL_NET_H

struct registers;

#define NET_HOST_MAX 128
#define MSG_DONTWAIT 1
#define NET_OPT_TTL 1

void syscall_net_connect(struct registers *r);
void syscall_net_send(struct registers *r);
void syscall_net_recv(struct registers *r);
void syscall_net_close(struct registers *r);
void syscall_net_setopt(struct registers *r);
void syscall_net_ifconfig(struct registers *r);
void syscall_net_setrtc(struct registers *r);
void syscall_net_gettime(struct registers *r);
void syscall_net_settime(struct registers *r);
void syscall_net_adjtime(struct registers *r);
void syscall_net_listen(struct registers *r);
void syscall_net_accept(struct registers *r);
void syscall_net_bind(struct registers *r);

#endif

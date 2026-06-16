/* builtins.h — the built-in service registrars, called by services_init().
 * Private to the services package; each is implemented in its own svc_*.c
 * (or, for sshd, in kernel/sshd.c). The only place the manager lists services. */
#ifndef OWOS_SERVICES_BUILTINS_H
#define OWOS_SERVICES_BUILTINS_H

void smbd_register(void);        /* svc_smbd.c       — SMB2 file server     */
void sshd_register(void);        /* kernel/sshd.c    — SSH-2 server         */
void dhcp_register(void);        /* svc_dhcp.c       — DHCP client          */
void ntp_register(void);         /* svc_ntp.c        — SNTP clock sync      */

#endif /* OWOS_SERVICES_BUILTINS_H */

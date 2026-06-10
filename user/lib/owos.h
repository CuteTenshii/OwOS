/* owos.h — the OwOS user-space library (minimal libc over int 0x80). */
#ifndef OWOS_USER_H
#define OWOS_USER_H

/* Thin syscall wrappers. */
int      write(int fd, const void *buf, int len);   /* bytes written          */
int      read(int fd, void *buf, int len);          /* bytes read (0 = EOF)   */
int      pipe(int fds[2]);                    /* fds[0]=read end, fds[1]=write  */
int      close(int fd);
void     exit(int code) __attribute__((noreturn));
int      fork(void);                          /* 0 in child, child pid in parent */
int      execp(const char *name);             /* replace this image; -1 on error */
int      waitpid(int pid);                     /* block for a child; its status  */
unsigned uptime(void);                        /* seconds since boot            */
int      readfile(const char *name, char *buf, int max);  /* bytes, or -1     */
int      writefile(const char *name, const void *buf, int len); /* 0, or -1    */

/* Networking: a tiny socket-ish API. net_connect resolves `host`, opens TCP,
 * and (if tls) runs the TLS handshake; it returns a handle >= 0, or a negative
 * error (-1 no NIC/slot, -2 resolve failed, -3 connect failed). net_recv
 * blocks and returns >0 bytes, 0 at EOF, -1 on error. */
int      net_connect(const char *host, int port, int tls);
int      net_send(int h, const void *buf, int len);
int      net_recv(int h, void *buf, int max);
int      net_close(int h);

/* Conveniences built on the above. */
void puts(const char *s);                     /* write a NUL-terminated string */
void print_uint(unsigned v);                  /* write a decimal number        */
int  readline(char *buf, int max);            /* echoed line input, returns len*/

#endif /* OWOS_USER_H */

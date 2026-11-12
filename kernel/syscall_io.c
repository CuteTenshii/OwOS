#include "syscall_io.h"
#include "console.h"
#include "file.h"
#include "idt.h"
#include "ipc.h"
#include "keyboard.h"
#include "pipe.h"
#include "sched.h"
#include "syscall_nr.h"
#include "uaccess.h"
#include <stdint.h>

/* Blocking pipe ends yield, and a sibling thread may unmap the user buffer meanwhile: stage the bytes in the kernel
 * and touch user memory only through copyin/copyout. */
#define IO_CHUNK 512

/* Write `len` user bytes at `ubuf` to pipe `id`, CRLF-translating when `tty`. Bytes written, or -1. */
static int pipe_write_user(int id, uint32_t ubuf, uint32_t len, bool tty)
{
  char kbuf[IO_CHUNK];
  uint32_t done = 0;
  while (done < len) {
    uint32_t n = len - done < IO_CHUNK ? len - done : IO_CHUNK;
    if (copyin(kbuf, ubuf + done, n) != 0)
      return done ? (int)done : -1;
    if (tty) {
      uint32_t from = 0;
      for (uint32_t i = 0; i <= n; i++) {
        if (i < n && kbuf[i] != '\n')
          continue;
        pipe_write(id, kbuf + from, (int)(i - from));
        if (i < n)
          pipe_write(id, "\r\n", 2);
        from = i + 1;
      }
      done += n;
      continue;
    }
    int w = pipe_write(id, kbuf, (int)n);
    if (w < 0)
      return done ? (int)done : -1;
    done += (uint32_t)w;
    if ((uint32_t)w < n)
      break;
  }
  return (int)done;
}

void syscall_io_write(struct registers *r)
{
  int fd = (int)r->ebx;
  uint32_t len = r->edx;
  if (fd < 0 || fd >= MAX_FD || !user_range(r->ecx, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  const char *buf = (const char *)r->ecx;
  struct fd *f = &sched_fds()[fd];
  if (f->type == FD_CONSOLE) {
    for (uint32_t i = 0; i < len; i++)
      con_putc(buf[i]);
    r->eax = len;
  } else if (f->type == FD_PIPE_WR) {
    r->eax = (uint32_t)pipe_write_user(f->pipe, r->ecx, len, false);
  } else if (f->type == FD_TTY) {
    /* output processing of a terminal: newline becomes carriage return + newline */
    int w = pipe_write_user(f->tx, r->ecx, len, true);
    r->eax = w < 0 ? (uint32_t)-1 : len;
  } else if (fd_is_channel(f)) {
    r->eax = (uint32_t)pipe_write_user(f->tx, r->ecx, len, false);
  } else {
    r->eax = (uint32_t)-1;
  }
}

void syscall_io_read(struct registers *r)
{
  int fd = (int)r->ebx;
  uint32_t len = r->edx;
  if (fd < 0 || fd >= MAX_FD || len == 0 || !user_out_range(r->ecx, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct fd *f = &sched_fds()[fd];
  char kbuf[IO_CHUNK];
  if (len > IO_CHUNK)
    len = IO_CHUNK; /* a pipe never holds more, and short reads are allowed */
  if (f->type == FD_CONSOLE) {
    char c;
    int got = 0;
    __asm__ volatile("sti");
    while (!(got = keyboard_poll_text(&c, 0))) {
      if (sched_intr_pending())
        break;
      __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");
    if (!got) {
      r->eax = (uint32_t)-1;
      return;
    }
    uint32_t n = 0;
    kbuf[n++] = c;
    while (n < len && keyboard_poll_text(&c, 0))
      kbuf[n++] = c;
    r->eax = copyout(r->ecx, kbuf, n) == 0 ? n : (uint32_t)-1;
  } else if (f->type == FD_PIPE_RD || fd_is_channel(f)) {
    int n = pipe_read(f->pipe, kbuf, (int)len);
    if (n > 0 && copyout(r->ecx, kbuf, (uint32_t)n) != 0)
      n = -1;
    r->eax = (uint32_t)n;
  } else {
    r->eax = (uint32_t)-1;
  }
}

void syscall_io_pipe(struct registers *r)
{
  if (!user_range(r->ebx, 8)) {
    r->eax = (uint32_t)-1;
    return;
  }
  int id = pipe_alloc();
  if (id < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct fd *fds = sched_fds();
  int rfd = -1, wfd = -1;
  for (int i = 0; i < MAX_FD; i++)
    if (fds[i].type == FD_NONE) {
      rfd = i;
      break;
    }
  for (int i = 0; i < MAX_FD; i++)
    if (fds[i].type == FD_NONE && i != rfd) {
      wfd = i;
      break;
    }
  if (rfd < 0 || wfd < 0) {
    pipe_close(id, 0);
    pipe_close(id, 1);
    r->eax = (uint32_t)-1;
    return;
  }
  fds[rfd].type = FD_PIPE_RD;
  fds[rfd].pipe = (uint8_t)id;
  fds[wfd].type = FD_PIPE_WR;
  fds[wfd].pipe = (uint8_t)id;
  int out[2] = {rfd, wfd};
  if (copyout(r->ebx, out, sizeof out) != 0) {
    pipe_close(id, 0);
    pipe_close(id, 1);
    fds[rfd].type = FD_NONE;
    fds[wfd].type = FD_NONE;
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = 0;
}

void syscall_io_close(struct registers *r)
{
  int fd = (int)r->ebx;
  if (fd < 0 || fd >= MAX_FD) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct fd *f = &sched_fds()[fd];
  if (f->type == FD_PIPE_RD)
    pipe_close(f->pipe, 0);
  else if (f->type == FD_PIPE_WR)
    pipe_close(f->pipe, 1);
  else if (fd_is_channel(f)) {
    pipe_close(f->pipe, 0);
    pipe_close(f->tx, 1);
  } else if (f->type == FD_PORT) {
    ipc_port_destroy(f->pipe);
  }
  f->type = FD_NONE;
  r->eax = 0;
}

void syscall_io_read_nb(struct registers *r)
{
  int fd = (int)r->ebx;
  uint32_t len = r->edx;
  if (fd < 0 || fd >= MAX_FD || len == 0 || !user_out_range(r->ecx, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct fd *f = &sched_fds()[fd];
  r->eax = f->type == FD_PIPE_RD || fd_is_channel(f) ? (uint32_t)pipe_read_nb(f->pipe, (void *)r->ecx, (int)len)
                                                     : (uint32_t)-1;
}

void syscall_io_port_open(struct registers *r)
{
  const char *name = (const char *)r->ebx;
  int as_server = (int)r->ecx;
  if (!user_range(r->ebx, 16)) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct fd *fds = sched_fds();
  int nfd = -1;
  for (int i = 0; i < MAX_FD; i++)
    if (fds[i].type == FD_NONE) {
      nfd = i;
      break;
    }
  if (nfd < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (as_server) {
    int pid = ipc_port_create(name);
    if (pid < 0) {
      r->eax = (uint32_t)-1;
      return;
    }
    fds[nfd].type = FD_PORT;
    fds[nfd].pipe = (uint8_t)pid;
  } else {
    int rx = 0, tx = 0;
    if (ipc_connect(name, &rx, &tx) < 0) {
      r->eax = (uint32_t)-1;
      return;
    }
    fds[nfd].type = FD_CHANNEL;
    fds[nfd].pipe = (uint8_t)rx;
    fds[nfd].tx = (uint8_t)tx;
  }
  r->eax = (uint32_t)nfd;
}

void syscall_io_port_accept(struct registers *r)
{
  int lfd = (int)r->ebx;
  struct fd *fds = sched_fds();
  if (lfd < 0 || lfd >= MAX_FD || fds[lfd].type != FD_PORT) {
    r->eax = (uint32_t)-1;
    return;
  }
  int rx = 0, tx = 0;
  if (ipc_accept(fds[lfd].pipe, &rx, &tx) < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  int nfd = -1;
  for (int i = 0; i < MAX_FD; i++)
    if (fds[i].type == FD_NONE) {
      nfd = i;
      break;
    }
  if (nfd < 0) {
    pipe_close(rx, 0);
    pipe_close(tx, 1);
    r->eax = (uint32_t)-1;
    return;
  }
  fds[nfd].type = FD_CHANNEL;
  fds[nfd].pipe = (uint8_t)rx;
  fds[nfd].tx = (uint8_t)tx;
  r->eax = (uint32_t)nfd;
}

/* SYS_TTY: terminal queries and the pty state (size, foreground job) kept on the slave-to-master pipe. */
void syscall_io_tty(struct registers *r)
{
  int fd = (int)r->ebx;
  struct fd *fds = sched_fds();
  r->eax = (uint32_t)-1;
  if (fd < 0 || fd >= MAX_FD || fds[fd].type == FD_NONE)
    return;
  struct fd *f = &fds[fd];
  if (r->ecx == TTY_ISATTY) {
    r->eax = f->type == FD_CONSOLE || f->type == FD_TTY || f->type == FD_PTY;
    return;
  }
  if (f->type != FD_TTY && f->type != FD_PTY)
    return;
  struct pipe_tty *t = pipe_tty(f->type == FD_PTY ? f->pipe : f->tx);
  switch (r->ecx) {
  case TTY_GETSIZE:
    r->eax = (uint32_t)t->cols << 16 | t->rows;
    break;
  case TTY_SETSIZE:
    if (f->type == FD_PTY && (r->edx >> 16) && (r->edx & 0xFFFF)) {
      t->cols = (uint16_t)(r->edx >> 16);
      t->rows = (uint16_t)r->edx;
      r->eax = 0;
    }
    break;
  case TTY_SETFG:
    t->fg_pid = (int)r->edx;
    r->eax = 0;
    break;
  case TTY_GETFG:
    r->eax = (uint32_t)t->fg_pid;
    break;
  }
}

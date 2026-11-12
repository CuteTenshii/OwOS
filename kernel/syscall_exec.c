#include "syscall_exec.h"
#include "file.h"
#include "fs.h"
#include "heap.h"
#include "idt.h"
#include "loader.h"
#include "paging.h"
#include "perm.h"
#include "pipe.h"
#include "sched.h"
#include "syscall_nr.h"
#include "syscall_path.h"
#include "uaccess.h"
#include "users.h"
#include <stdint.h>

void syscall_exec_replace(struct registers *r)
{
  char name[24], arg[128], abs[SYS_PATH_MAX];
  int have_arg = 0;
  if (!user_range(r->ebx, 24) || copyin_str(r->ebx, name, sizeof name) != 0 ||
      (r->ecx && (copyin_str(r->ecx, arg, sizeof arg) != 0))) {
    r->eax = (uint32_t)-1;
    return;
  }
  have_arg = r->ecx != 0;
  if (resolve_path(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  int acc = fs_access(abs, FS_X);
  if (acc != 0) {
    r->eax = (uint32_t)acc;
    return;
  }
  int size = fs_size(abs);
  if (size <= 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  void *image = kmalloc((uint32_t)size);
  if (!image) {
    r->eax = (uint32_t)-1;
    return;
  }
  int n = fs_read(abs, image, (uint32_t)size);
  uint32_t *dir, entry, brk;
  if (n < 0 || load_elf(image, (uint32_t)n, &dir, &entry, &brk) != 0) {
    kfree(image);
    r->eax = (uint32_t)-1;
    return;
  }
  kfree(image);
  char *argv[2] = {name, arg};
  uint32_t user_esp = setup_args(dir, have_arg ? 2 : 1, argv);
  if (sched_replace(dir, entry, user_esp, brk, r) < 0) {
    paging_free_process_dir(dir);
    r->eax = (uint32_t)-1;
  }
}

void syscall_exec_spawn(struct registers *r)
{
  char raw[SYS_PATH_MAX], path[SYS_PATH_MAX], arg[SYS_PATH_MAX];
  if (copyin_str(r->ebx, raw, sizeof raw) != 0 || path_join("/", raw, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  int acc = fs_access(path, FS_X);
  if (acc != 0) {
    r->eax = (uint32_t)acc;
    return;
  }
  int size = fs_size(path);
  if (size < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint8_t *image = kmalloc((uint32_t)size);
  if (!image) {
    r->eax = (uint32_t)-1;
    return;
  }
  int n = fs_read(path, image, (uint32_t)size);
  if (n < 0) {
    kfree(image);
    r->eax = (uint32_t)-1;
    return;
  }
  char *argv[2] = {path, arg};
  int argc = 1;
  if (r->ecx) {
    if (copyin_str(r->ecx, arg, sizeof arg) != 0) {
      kfree(image);
      r->eax = (uint32_t)-1;
      return;
    }
    argc = 2;
  }
  int pid = exec_program(image, (uint32_t)n, argc, argv);
  kfree(image);
  if (pid >= 0)
    sched_set_cwd(pid, sched_cwd());
  r->eax = (uint32_t)pid;
}
void syscall_exec_spawn_cap(struct registers *r)
{
  /* Args arrive as one packed request so the shell can pass a full argv vector (curl --http2 <url>): it
   * tokenizes in user space, the kernel just marshals the strings in and never parses a command line.
   *
   * Child stdio, by mode:
   *   default      fd 1 is a pipe whose read end comes back in *rfd; with a non-zero `wfd`, fd 0 is a second pipe
   *                whose write end comes back in *wfd (an interactive child such as ssh).
   *   SPAWN_STDIO  fds 0-2 are copies of the caller's fds named in stdio[] (-1: left as the default). Nothing is
   * returned. SPAWN_PTY    fds 0-2 are the slave side of a new pty; the master fd comes back in *rfd. uid/gid/cwd are
   * optional: uid<0/gid<0/cwd==0 mean "inherit". PRIVILEGE RULE: only a caller running as root may hand a child a
   * uid/gid different from its own. This is the primitive that lets /bin/sshd drop from root to the authenticated user
   * for a login session. */
  struct cap_req {
    uint32_t path;
    uint32_t argv;
    int argc;
    uint32_t rfd;
    uint32_t wfd;
    int uid;
    int gid;
    uint32_t cwd;
    uint32_t flags;
    int stdio[3];
    uint32_t env;
    uint32_t env_len;
  };
  if (!user_range(r->ebx, sizeof(struct cap_req))) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct cap_req req;
  if (copyin(&req, r->ebx, sizeof req) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  bool pty = req.flags & SPAWN_PTY, dup_stdio = req.flags & SPAWN_STDIO, set_env = req.flags & SPAWN_ENV;
  bool want_rfd = !dup_stdio; /* the capture pipe, or the pty master */
  if (!user_range(req.path, 1) || (want_rfd && !user_out_range(req.rfd, 4)) || (pty && dup_stdio)) {
    r->eax = (uint32_t)-1;
    return;
  }
  int want_stdin = !pty && !dup_stdio && req.wfd != 0;
  if (want_stdin && !user_out_range(req.wfd, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (set_env && !user_range(req.env, req.env_len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (req.cwd && !user_range(req.cwd, 1)) {
    r->eax = (uint32_t)-1;
    return;
  }

  /* Copy the optional cwd out of user space (before we touch a new address space). Enforce the privilege rule up
   * front so we never spawn a child we would then have to tear down. */
  uint32_t caller_uid = sched_uid();
  int set_uid = req.uid >= 0, set_gid = req.gid >= 0;
  bool to_root = (!set_uid || req.uid == 0) && (!set_gid || req.gid == 0);
  if (caller_uid != 0 && !(to_root && users_sudo_valid(caller_uid)) &&
      ((set_uid && (uint32_t)req.uid != caller_uid) || (set_gid && (uint32_t)req.gid != sched_gid()))) {
    r->eax = (uint32_t)-1; /* non-root may not change creds, except to root under a sudo grant */
    return;
  }
  char cwdbuf[SYS_PATH_MAX];
  if (req.cwd) {
    if (copyin_str(req.cwd, cwdbuf, sizeof cwdbuf) != 0) {
      r->eax = (uint32_t)-1;
      return;
    }
  }

  char raw[SYS_PATH_MAX], path[SYS_PATH_MAX];
  if (copyin_str(req.path, raw, sizeof raw) != 0 || path_join("/", raw, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }

/* Marshal argv: up to MAXARGS strings, each MAXARG bytes, copied out of the user image (bounds-checked per byte)
 * before we touch a new address space. */
#define MAXARGS 16
#define MAXARG 192
  static char argbuf[MAXARGS][MAXARG]; /* syscalls don't re-enter on this OS */
  char *argv[MAXARGS];
  int argc = req.argc;
  if (argc < 1)
    argc = 1;
  if (argc > MAXARGS)
    argc = MAXARGS;
  argv[0] = path;
  for (int ai = 1; ai < argc; ai++) {
    argv[ai] = argbuf[ai];
    argbuf[ai][0] = '\0';
    uint32_t pp = req.argv + (uint32_t)ai * 4;
    if (!user_range(pp, 4)) {
      argc = ai;
      break;
    }
    uint32_t sp;
    if (copyin(&sp, pp, sizeof sp) != 0 || copyin_str(sp, argbuf[ai], MAXARG) != 0) {
      argc = ai;
      break;
    }
  }

  int acc = fs_access(path, FS_X);
  if (acc != 0) {
    r->eax = (uint32_t)acc;
    return;
  }
  int fsz = fs_size(path);
  if (fsz < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  /* Reserve the caller's fd slot and the pipes before loading the image, so a failure here doesn't leak a
   * half-spawned child. */
  struct fd *fds = sched_fds();
  int rfd = -1, id = -1, id2 = -1;
  struct fd src[3] = {0};
  if (want_rfd) {
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        rfd = i;
        break;
      }
    if (rfd < 0) {
      r->eax = (uint32_t)-1;
      return;
    }
    id = pipe_alloc();
    id2 = pty ? pipe_alloc() : -1;
    if (id < 0 || (pty && id2 < 0)) {
      if (id >= 0) {
        pipe_close(id, 0);
        pipe_close(id, 1);
      }
      r->eax = (uint32_t)-1;
      return;
    }
  } else {
    for (int i = 0; i < 3; i++) {
      int n = req.stdio[i];
      if (n >= 0 && (n >= MAX_FD || fds[n].type == FD_NONE || fds[n].type == FD_PORT)) {
        r->eax = (uint32_t)-1;
        return;
      }
      if (n >= 0)
        src[i] = fds[n];
    }
  }
  uint8_t *buf = kmalloc((uint32_t)fsz);
  int sz = buf ? fs_read(path, buf, (uint32_t)fsz) : -1;
  int pid = sz < 0 ? -1 : exec_program(buf, (uint32_t)sz, argc, argv);
  kfree(buf);
  if (pid < 0) {
    if (id >= 0) {
      pipe_close(id, 0);
      pipe_close(id, 1);
    }
    if (id2 >= 0) {
      pipe_close(id2, 0);
      pipe_close(id2, 1);
    }
    r->eax = (uint32_t)-1;
    return;
  }
  if (pty) {
    /* id carries the master-to-slave bytes, id2 the slave-to-master bytes and the terminal state. Each fd below
     * takes its own reference; the creation-time ones are dropped after. */
    struct fd slave = {.type = FD_TTY, .pipe = (uint8_t)id, .tx = (uint8_t)id2};
    struct fd master = {.type = FD_PTY, .pipe = (uint8_t)id2, .tx = (uint8_t)id};
    for (int i = 0; i < 3; i++)
      sched_set_fd(pid, i, &slave);
    fds[rfd] = master;
    fd_ref(&master);
    pipe_close(id, 0);
    pipe_close(id, 1);
    pipe_close(id2, 0);
    pipe_close(id2, 1);
  } else if (dup_stdio) {
    for (int i = 0; i < 3; i++)
      if (req.stdio[i] >= 0)
        sched_set_fd(pid, i, &src[i]);
  } else {
    fds[rfd].type = FD_PIPE_RD;
    fds[rfd].pipe = (uint8_t)id;      /* our read end   */
    sched_set_pipe_fd(pid, 1, 1, id); /* child fd 1 (stdout) -> write end */
  }
  /* Creds + cwd, atomic with the fd wiring (the int-0x80 gate runs IF=0, so no timer tick runs the child before it
   * is fully set up). */
  if (set_uid || set_gid)
    sched_set_creds(pid, set_uid ? (uint32_t)req.uid : sched_uid(), set_gid ? (uint32_t)req.gid : sched_gid());
  sched_set_cwd(pid, req.cwd ? cwdbuf : sched_cwd()); /* explicit cwd, else inherit */
  if (set_env) {
    char *block = kmalloc(req.env_len ? req.env_len : 1);
    if (block && copyin(block, req.env, req.env_len) == 0)
      sched_set_env_block(pid, block, req.env_len);
    kfree(block);
  }
  if (want_rfd && copyout(req.rfd, &rfd, sizeof rfd) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  /* Optional stdin pipe: our write end -> child fd 0 (its read end). If we can't allocate it, the child still
   * runs (stdout-only); we just report no write fd. The child already exists, so don't fail the whole spawn. */
  if (want_stdin) {
    int wfd = -1;
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        wfd = i;
        break;
      }
    int id3 = wfd >= 0 ? pipe_alloc() : -1;
    if (wfd >= 0 && id3 >= 0) {
      fds[wfd].type = FD_PIPE_WR;
      fds[wfd].pipe = (uint8_t)id3;
      sched_set_pipe_fd(pid, 0, 0, id3); /* child fd 0 (stdin) -> read end */
      copyout(req.wfd, &wfd, sizeof wfd);
    } else {
      if (id3 >= 0) {
        pipe_close(id3, 0);
        pipe_close(id3, 1);
      }
      int nofd = -1;
      copyout(req.wfd, &nofd, sizeof nofd);
    }
  }
  r->eax = (uint32_t)pid;
#undef MAXARGS
#undef MAXARG
}

#include "syscall_exec.h"
#include "file.h"
#include "fs.h"
#include "heap.h"
#include "idt.h"
#include "loader.h"
#include "paging.h"
#include "pipe.h"
#include "sched.h"
#include "syscall_path.h"
#include "uaccess.h"
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
  char path[SYS_PATH_MAX], arg[SYS_PATH_MAX];
  if (copyin_str(r->ebx, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
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
  /* Args arrive as one packed request so the shell can pass a full argv
   * vector (curl --http2 <url>) -- it tokenizes (with quotes) in user space,
   * the kernel just marshals the strings in and never parses a command line.
   */
  /* `wfd` is optional (0 = stdout-only capture, as the CLI tools use). When
   * non-zero, also wire the child's stdin (fd 0) to a second pipe and hand
   * the *write* end back in *wfd, so the terminal can feed keystrokes to an
   * interactive child (ssh). All callers pass the 5-field struct. */
  /* uid/gid/cwd are optional: uid<0/gid<0/cwd==0 mean "inherit" (the child
   * keeps the caller's identity/cwd, the historical behavior). PRIVILEGE
   * RULE: only a caller running as root (uid 0) may hand a child a uid/gid
   * different from its own; a non-root caller asking for a different uid or
   * gid is rejected. This is the primitive that lets /bin/sshd drop from
   * root to the authenticated user for a login session. */
  struct cap_req {
    uint32_t path;
    uint32_t argv;
    int argc;
    uint32_t rfd;
    uint32_t wfd;
    int uid;
    int gid;
    uint32_t cwd;
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
  if (!user_range(req.path, 1) || !user_out_range(req.rfd, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  int want_stdin = req.wfd != 0;
  if (want_stdin && !user_out_range(req.wfd, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (req.cwd && !user_range(req.cwd, 1)) {
    r->eax = (uint32_t)-1;
    return;
  }

  /* Copy the optional cwd out of user space (before we touch a new address
   * space). Enforce the privilege rule up front so we never spawn a child we
   * would then have to tear down. */
  uint32_t caller_uid = sched_uid();
  int set_uid = req.uid >= 0, set_gid = req.gid >= 0;
  if (caller_uid != 0 &&
      ((set_uid && (uint32_t)req.uid != caller_uid) || (set_gid && (uint32_t)req.gid != sched_gid()))) {
    r->eax = (uint32_t)-1; /* non-root may not change creds */
    return;
  }
  char cwdbuf[SYS_PATH_MAX];
  if (req.cwd) {
    if (copyin_str(req.cwd, cwdbuf, sizeof cwdbuf) != 0) {
      r->eax = (uint32_t)-1;
      return;
    }
  }

  char path[SYS_PATH_MAX];
  if (copyin_str(req.path, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }

/* Marshal argv: up to MAXARGS strings, each MAXARG bytes, copied out of the
 * user image (bounds-checked per byte) before we touch a new address space. */
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

  int fsz = fs_size(path);
  if (fsz < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  /* Reserve a free fd and a pipe before loading the image, so a failure here
   * doesn't leak a half-spawned child. */
  struct fd *fds = sched_fds();
  int rfd = -1;
  for (int i = 0; i < MAX_FD; i++)
    if (fds[i].type == FD_NONE) {
      rfd = i;
      break;
    }
  if (rfd < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  int id = pipe_alloc();
  if (id < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint8_t *buf = kmalloc((uint32_t)fsz);
  if (!buf) {
    pipe_close(id, 0);
    pipe_close(id, 1);
    r->eax = (uint32_t)-1;
    return;
  }
  int sz = fs_read(path, buf, (uint32_t)fsz);
  int pid = sz < 0 ? -1 : exec_program(buf, (uint32_t)sz, argc, argv);
  kfree(buf);
  if (pid < 0) {
    pipe_close(id, 0);
    pipe_close(id, 1);
    r->eax = (uint32_t)-1;
    return;
  }
  fds[rfd].type = FD_PIPE_RD;
  fds[rfd].pipe = (uint8_t)id;      /* our read end   */
  sched_set_pipe_fd(pid, 1, 1, id); /* child fd 1 (stdout) -> write end */
  /* Creds + cwd, atomic with the fd wiring (the int-0x80 gate runs IF=0, so
   * no timer tick runs the child before it is fully set up). */
  if (set_uid || set_gid)
    sched_set_creds(pid, set_uid ? (uint32_t)req.uid : sched_uid(), set_gid ? (uint32_t)req.gid : sched_gid());
  sched_set_cwd(pid, req.cwd ? cwdbuf : sched_cwd()); /* explicit cwd, else inherit */
  if (copyout(req.rfd, &rfd, sizeof rfd) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  /* Optional stdin pipe: our write end -> child fd 0 (its read end). If we
   * can't allocate it, the child still runs (stdout-only); we just report no
   * write fd. The child already exists, so don't fail the whole spawn. */
  if (want_stdin) {
    int wfd = -1;
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        wfd = i;
        break;
      }
    int id2 = wfd >= 0 ? pipe_alloc() : -1;
    if (wfd >= 0 && id2 >= 0) {
      fds[wfd].type = FD_PIPE_WR;
      fds[wfd].pipe = (uint8_t)id2;
      sched_set_pipe_fd(pid, 0, 0, id2); /* child fd 0 (stdin) -> read end */
      copyout(req.wfd, &wfd, sizeof wfd);
    } else {
      if (id2 >= 0) {
        pipe_close(id2, 0);
        pipe_close(id2, 1);
      }
      int nofd = -1;
      copyout(req.wfd, &nofd, sizeof nofd);
    }
  }
  r->eax = (uint32_t)pid;
#undef MAXARGS
#undef MAXARG
  return;
}

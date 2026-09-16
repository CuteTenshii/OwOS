#include "syscall_fs.h"
#include "fs.h"
#include "idt.h"
#include "procfs.h"
#include "sched.h"
#include "syscall_path.h"
#include "sysfs.h"
#include "uaccess.h"
#include <stdint.h>

void syscall_fs_stream_begin(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  join_cwd(name, abs, sizeof abs);
  r->eax = (uint32_t)fs_stream_begin(abs);
}

void syscall_fs_stream_write(struct registers *r)
{
  uint32_t buf = r->ebx, len = r->ecx;
  if (!user_range(buf, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)fs_stream_write((const void *)buf, len);
}

void syscall_fs_stream_end(struct registers *r) { r->eax = (uint32_t)fs_stream_end(); }

static void simple_path_op(struct registers *r, int op)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  join_cwd(name, abs, sizeof abs);
  if (op == 0)
    r->eax = (uint32_t)fs_mkdir(abs);
  else if (op == 1)
    r->eax = (uint32_t)fs_rmdir(abs);
  else
    r->eax = (uint32_t)fs_delete(abs);
}

void syscall_fs_mkdir(struct registers *r) { simple_path_op(r, 0); }

void syscall_fs_rmdir(struct registers *r) { simple_path_op(r, 1); }

void syscall_fs_delete(struct registers *r) { simple_path_op(r, 2); }

void syscall_fs_rename(struct registers *r)
{
  char old_name[SYS_PATH_MAX], new_name[SYS_PATH_MAX];
  char old_path[SYS_PATH_MAX], new_path[SYS_PATH_MAX];
  if (copyin_str(r->ebx, old_name, sizeof old_name) != 0 || copyin_str(r->ecx, new_name, sizeof new_name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  join_cwd(old_name, old_path, sizeof old_path);
  join_cwd(new_name, new_path, sizeof new_path);
  r->eax = (uint32_t)fs_rename(old_path, new_path);
}

void syscall_fs_chdir(struct registers *r)
{
  char path[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  int is_dir = 0;
  if (copyin_str(r->ebx, path, sizeof path) != 0 || resolve_path(path, abs, sizeof abs) != 0 ||
      fs_stat(abs, &is_dir, 0, 0) != 0 || !is_dir) {
    r->eax = (uint32_t)-1;
    return;
  }
  sched_set_cwd_self(abs);
  r->eax = 0;
}

void syscall_fs_readfile(struct registers *r)
{
  char name[SYS_PATH_MAX];
  uint32_t buf = r->ecx, max = r->edx;
  if (copyin_str(r->ebx, name, sizeof name) != 0 || !user_out_range(buf, max)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)read_with_path(name, (void *)buf, max);
}

void syscall_fs_writefile(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  uint32_t buf = r->ecx, len = r->edx;
  if (copyin_str(r->ebx, name, sizeof name) != 0 || !user_range(buf, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  join_cwd(name, abs, sizeof abs);
  const char *proc = procfs_subpath(abs);
  if (proc && proc[0]) {
    r->eax = (uint32_t)procfs_write(proc, (const char *)buf, (int)len);
    return;
  }
  const char *sys = sysfs_subpath(abs);
  if (sys && sys[0]) {
    r->eax = (uint32_t)sysfs_write(sys, (const char *)buf, (int)len);
    return;
  }
  r->eax = (uint32_t)fs_create(abs, (const void *)buf, len);
}

void syscall_fs_readfile_at(struct registers *r)
{
  uint32_t args[4];
  char name[SYS_PATH_MAX];
  if (!user_range(r->ebx, sizeof args) || copyin(args, r->ebx, sizeof args) != 0 ||
      copyin_str(args[0], name, sizeof name) != 0 || (args[3] && !user_out_range(args[2], args[3]))) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)read_at_with_path(name, args[1], (void *)args[2], args[3]);
}

#include "syscall_fs.h"
#include "blk.h"
#include "fs.h"
#include "idt.h"
#include "kstring.h"
#include "procfs.h"
#include "rtc.h"
#include "sched.h"
#include "syscall_path.h"
#include "sysfs.h"
#include "uaccess.h"
#include <stdint.h>

#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

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

void syscall_fs_rtc_epoch(struct registers *r) { r->eax = rtc_epoch(); }

void syscall_fs_readdir(struct registers *r)
{
  struct rd_out {
    char name[64];
    int is_dir;
    uint32_t size;
  } *out;
  char path[SYS_PATH_MAX], name[64];
  int is_dir = 0;
  uint32_t size = 0;
  if (!user_out_range(r->edx, sizeof *out) || copyin_str(r->ebx, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  int got = fs_enum_dir(path[0] ? path : "/", (int)r->ecx, name, &is_dir, &size, 0);
  if (got) {
    out = (struct rd_out *)r->edx;
    int i = 0;
    for (; i < (int)sizeof out->name - 1 && name[i]; i++)
      out->name[i] = name[i];
    out->name[i] = '\0';
    out->is_dir = is_dir;
    out->size = size;
  }
  r->eax = (uint32_t)got;
}

void syscall_fs_volumes(struct registers *r)
{
  struct vinfo {
    char dev[16];
    char kind[8];
    char label[24];
    char point[40];
    int mounted;
  } *out;
  if (!user_out_range(r->ecx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  out = (struct vinfo *)r->ecx;
  struct blk_devinfo devs[8];
  int n = blk_dev_list(devs, 8), idx = (int)r->ebx;
  if (idx < 0 || idx >= n) {
    r->eax = 0;
    return;
  }
  struct blk_devinfo *dev = &devs[idx];
  const char *point = "", *label = dev->model;
  int mounted = 0;
  struct fs_mountinfo mount;
  for (int i = 0; fs_mount_enum(i, &mount); i++)
    if (streq(mount.dev, dev->dev)) {
      mounted = 1;
      point = mount.point;
      if (mount.label && mount.label[0])
        label = mount.label;
      break;
    }
  if (!label || !label[0])
    label = dev->kind;
  int i = 0;
  for (; i < 15 && dev->dev[i]; i++)
    out->dev[i] = dev->dev[i];
  out->dev[i] = '\0';
  for (i = 0; i < 7 && dev->kind[i]; i++)
    out->kind[i] = dev->kind[i];
  out->kind[i] = '\0';
  for (i = 0; i < 23 && label[i]; i++)
    out->label[i] = label[i];
  out->label[i] = '\0';
  for (i = 0; i < 39 && point[i]; i++)
    out->point[i] = point[i];
  out->point[i] = '\0';
  out->mounted = mounted;
  r->eax = 1;
}

void syscall_fs_mount(struct registers *r)
{
  char dev[16], point[48];
  if (copyin_str(r->ebx, dev, sizeof dev) != 0 || copyin_str(r->ecx, point, sizeof point) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  __asm__ volatile("sti");
  r->eax = (uint32_t)fs_mount(dev, point);
}

void syscall_fs_umount(struct registers *r)
{
  char point[48];
  if (copyin_str(r->ebx, point, sizeof point) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)fs_unmount(point);
}

void syscall_fs_stat(struct registers *r)
{
  struct st_out {
    int is_dir;
    uint32_t size;
    uint32_t mtime;
  } *out;
  char name[SYS_PATH_MAX], path[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0 || !user_out_range(r->ecx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  join_cwd(name, path, sizeof path);
  int is_dir = 0;
  uint32_t size = 0, mtime = 0;
  int rc = fs_stat(path, &is_dir, &size, &mtime);
  if (rc == 0) {
    out = (struct st_out *)r->ecx;
    out->is_dir = is_dir;
    out->size = size;
    out->mtime = mtime;
  }
  r->eax = (uint32_t)rc;
}

void syscall_fs_statfs(struct registers *r)
{
  struct sfs_out {
    uint64_t total;
    uint64_t free;
  } *out;
  if (!user_out_range(r->ecx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint64_t total = (uint64_t)blk_sectors() * 512;
  uint64_t used = fs_used_bytes();
  out = (struct sfs_out *)r->ecx;
  out->total = total;
  out->free = total > used ? total - used : 0;
  r->eax = 0;
}

void syscall_fs_uname(struct registers *r)
{
  struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65];
  } *out;
  if (!user_out_range(r->ebx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  out = (struct utsname *)r->ebx;
  memset(out, 0, sizeof *out);
  strlcpy(out->sysname, "OwOS", sizeof out->sysname);
  strlcpy(out->release, OWOS_VERSION, sizeof out->release);
  strlcpy(out->version, OWOS_VERSION, sizeof out->version);
#if defined(__x86_64__)
  strlcpy(out->machine, "x86_64", sizeof out->machine);
#else
  strlcpy(out->machine, "i386", sizeof out->machine);
#endif
  r->eax = 0;
}

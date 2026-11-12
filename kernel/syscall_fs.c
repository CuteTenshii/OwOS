#include "syscall_fs.h"
#include "blk.h"
#include "fs.h"
#include "fsmount.h"
#include "idt.h"
#include "kstring.h"
#include "perm.h"
#include "procfs.h"
#include "rtc.h"
#include "sched.h"
#include "syscall_path.h"
#include "sysfs.h"
#include "uaccess.h"
#include "wallclock.h"
#include <stdint.h>

#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

/* Fail with -EACCES if the caller may not do `want` on abs; other failures fall through. */
static bool access_denied(struct registers *r, const char *abs, int want)
{
  int rc = fs_access(abs, want);
  if (rc != -PERM_EACCES)
    return false;
  r->eax = (uint32_t)rc;
  return true;
}

void syscall_fs_stream_begin(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_W | FS_ACC_CREATE))
    return;
  r->eax = (uint32_t)fs_stream_begin(abs);
}

void syscall_fs_stream_write(struct registers *r)
{
  uint32_t buf = r->ebx, len = r->ecx;
  if (!user_range(buf, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)fs_stream_write((const void *)(uintptr_t)buf, len);
}

void syscall_fs_stream_end(struct registers *r) { r->eax = (uint32_t)fs_stream_end(); }

static void simple_path_op(struct registers *r, int op)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_ACC_ENTRY))
    return;
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
  if (join_cwd(old_name, old_path, sizeof old_path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  join_cwd(new_name, new_path, sizeof new_path);
  if (access_denied(r, old_path, FS_ACC_ENTRY) || access_denied(r, new_path, FS_ACC_ENTRY))
    return;
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
  if (access_denied(r, abs, FS_X))
    return;
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
  r->eax = (uint32_t)read_with_path(name, (void *)(uintptr_t)buf, max);
}

void syscall_fs_writefile(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  uint32_t buf = r->ecx, len = r->edx;
  if (copyin_str(r->ebx, name, sizeof name) != 0 || !user_range(buf, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_W | FS_ACC_CREATE))
    return;
  const char *proc = procfs_subpath(abs);
  if (proc && proc[0]) {
    r->eax = (uint32_t)procfs_write(proc, (const char *)(uintptr_t)buf, (int)len);
    return;
  }
  const char *sys = sysfs_subpath(abs);
  if (sys && sys[0]) {
    r->eax = (uint32_t)sysfs_write(sys, (const char *)(uintptr_t)buf, (int)len);
    return;
  }
  r->eax = (uint32_t)fs_create(abs, (const void *)(uintptr_t)buf, len);
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
  r->eax = (uint32_t)read_at_with_path(name, args[1], (void *)(uintptr_t)args[2], args[3]);
}

void syscall_fs_rtc_epoch(struct registers *r)
{
  struct timeval64 tv;
  wallclock_get(&tv);
  r->eax = (uint32_t)tv.tv_sec;
}

void syscall_fs_readdir(struct registers *r)
{
  struct rd_out {
    char name[64];
    int is_dir;
    uint32_t size;
  } *out;
  char raw[SYS_PATH_MAX], path[SYS_PATH_MAX], name[FS_ENUM_NAME_MAX];
  int is_dir = 0;
  uint32_t size = 0;
  /* readdir paths have always been root-relative; normalize them like every other path. */
  if (!user_out_range(r->edx, sizeof *out) || copyin_str(r->ebx, raw, sizeof raw) != 0 ||
      path_join("/", raw, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, path, FS_R))
    return;
  int got = fs_enum_dir(path, (int)r->ecx, name, &is_dir, &size, 0);
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
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
  } *out;
  char name[SYS_PATH_MAX], path[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0 || !user_out_range(r->ecx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, path, sizeof path) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, path, 0))
    return;
  struct fs_stat_info st;
  int rc = fs_stat_ex(path, &st);
  if (rc == 0) {
    out = (struct st_out *)r->ecx;
    out->is_dir = st.is_dir;
    out->size = st.size;
    out->mtime = st.mtime;
    out->mode = st.mode;
    out->uid = st.uid;
    out->gid = st.gid;
  }
  r->eax = (uint32_t)rc;
}

void syscall_fs_chmod(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  struct fs_stat_info st;
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, 0))
    return;
  if (fs_stat_ex(abs, &st) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (!perm_may_chmod(st.uid, sched_uid())) {
    r->eax = (uint32_t)-PERM_EACCES;
    return;
  }
  r->eax = (uint32_t)fs_chmod(abs, r->ecx);
}

/* uid/gid of -1 leave that field unchanged. */
void syscall_fs_chown(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  struct fs_stat_info st;
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, 0))
    return;
  if (fs_stat_ex(abs, &st) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint32_t uid = r->ecx == (uint32_t)-1 ? st.uid : r->ecx;
  uint32_t gid = r->edx == (uint32_t)-1 ? st.gid : r->edx;
  if (!perm_may_chown(st.uid, sched_uid(), sched_gid(), uid, gid)) {
    r->eax = (uint32_t)-PERM_EACCES;
    return;
  }
  r->eax = (uint32_t)fs_chown(abs, uid, gid);
}

/* A mask of -1 only queries. Returns the previous mask. */
void syscall_fs_umask(struct registers *r)
{
  r->eax = r->ebx == (uint32_t)-1 ? sched_umask() : sched_set_umask(r->ebx);
}

void syscall_fs_access(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(r->ebx, name, sizeof name) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)fs_access(abs, (int)(r->ecx & 7u));
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

#include "syscall_fs.h"
#include "blk.h"
#include "cpu.h"
#include "fs.h"
#include "fsmount.h"
#include "irqflags.h"
#include "kstring.h"
#include "osinstall.h"
#include "perm.h"
#include "procfs.h"
#include "regs.h"
#include "rtc.h"
#include "sched.h"
#include "syscall_path.h"
#include "sysfs.h"
#include "uaccess.h"
#include "users.h"
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
  SC_RET(r) = (uint32_t)rc;
  return true;
}

/* The single stream belongs to the process that began it; 0 when none is open. */
static uint32_t stream_owner;

static bool stream_mine(void) { return stream_owner != 0 && stream_owner == sched_current_process_id(); }

void syscall_fs_process_exiting(uint32_t pid)
{
  if (stream_owner == pid) {
    fs_stream_abort();
    stream_owner = 0;
  }
}

void syscall_fs_stream_begin(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_W | FS_ACC_CREATE))
    return;
  int rc = fs_stream_begin(abs);
  if (rc == 0)
    stream_owner = sched_current_process_id();
  SC_RET(r) = (uint32_t)rc;
}

void syscall_fs_stream_write(struct registers *r)
{
  uaddr_t buf = SC_ARG0(r);
  uint32_t len = (uint32_t)SC_ARG1(r);
  if (!stream_mine() || !user_range(buf, len)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)fs_stream_write((const void *)(uintptr_t)buf, len);
}

void syscall_fs_stream_end(struct registers *r)
{
  if (!stream_mine()) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  stream_owner = 0;
  SC_RET(r) = (uint32_t)fs_stream_end();
}

static void simple_path_op(struct registers *r, int op)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_ACC_ENTRY))
    return;
  if (op == 0)
    SC_RET(r) = (uint32_t)fs_mkdir(abs);
  else if (op == 1)
    SC_RET(r) = (uint32_t)fs_rmdir(abs);
  else
    SC_RET(r) = (uint32_t)fs_delete(abs);
}

void syscall_fs_mkdir(struct registers *r) { simple_path_op(r, 0); }

void syscall_fs_rmdir(struct registers *r) { simple_path_op(r, 1); }

void syscall_fs_delete(struct registers *r) { simple_path_op(r, 2); }

void syscall_fs_rename(struct registers *r)
{
  char old_name[SYS_PATH_MAX], new_name[SYS_PATH_MAX];
  char old_path[SYS_PATH_MAX], new_path[SYS_PATH_MAX];
  if (copyin_str(SC_ARG0(r), old_name, sizeof old_name) != 0 ||
      copyin_str(SC_ARG1(r), new_name, sizeof new_name) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(old_name, old_path, sizeof old_path) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  join_cwd(new_name, new_path, sizeof new_path);
  if (access_denied(r, old_path, FS_ACC_ENTRY) || access_denied(r, new_path, FS_ACC_ENTRY))
    return;
  SC_RET(r) = (uint32_t)fs_rename(old_path, new_path);
}

void syscall_fs_chdir(struct registers *r)
{
  char path[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  int is_dir = 0;
  if (copyin_str(SC_ARG0(r), path, sizeof path) != 0 || resolve_path(path, abs, sizeof abs) != 0 ||
      fs_stat(abs, &is_dir, 0, 0) != 0 || !is_dir) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_X))
    return;
  sched_set_cwd_self(abs);
  SC_RET(r) = 0;
}

void syscall_fs_readfile(struct registers *r)
{
  char name[SYS_PATH_MAX];
  uaddr_t buf = SC_ARG1(r);
  uint32_t max = (uint32_t)SC_ARG2(r);
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0 || !user_out_range(buf, max)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)read_with_path(name, (void *)(uintptr_t)buf, max);
}

void syscall_fs_writefile(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  uaddr_t buf = SC_ARG1(r);
  uint32_t len = (uint32_t)SC_ARG2(r);
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0 || !user_range(buf, len)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, FS_W | FS_ACC_CREATE))
    return;
  const char *proc = procfs_subpath(abs);
  if (proc && proc[0]) {
    SC_RET(r) = (uint32_t)procfs_write(proc, (const char *)(uintptr_t)buf, (int)len);
    return;
  }
  const char *sys = sysfs_subpath(abs);
  if (sys && sys[0]) {
    SC_RET(r) = (uint32_t)sysfs_write(sys, (const char *)(uintptr_t)buf, (int)len);
    return;
  }
  SC_RET(r) = (uint32_t)fs_create(abs, (const void *)(uintptr_t)buf, len);
}

void syscall_fs_readfile_at(struct registers *r)
{
  sys_word args[4];
  char name[SYS_PATH_MAX];
  if (!user_range(SC_ARG0(r), sizeof args) || copyin(args, SC_ARG0(r), sizeof args) != 0 ||
      copyin_str(args[0], name, sizeof name) != 0 || (args[3] && !user_out_range(args[2], (uint32_t)args[3]))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)read_at_with_path(name, (uint32_t)args[1], (void *)(uintptr_t)args[2], (uint32_t)args[3]);
}

void syscall_fs_rtc_epoch(struct registers *r)
{
  struct timeval64 tv;
  wallclock_get(&tv);
  SC_RET(r) = (uint32_t)tv.tv_sec;
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
  if (!user_out_range(SC_ARG2(r), sizeof *out) || copyin_str(SC_ARG0(r), raw, sizeof raw) != 0 ||
      path_join("/", raw, path, sizeof path) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, path, FS_R))
    return;
  int got = fs_enum_dir(path, (int)SC_ARG1(r), name, &is_dir, &size, 0);
  if (got) {
    out = (struct rd_out *)SC_ARG2(r);
    int i = 0;
    for (; i < (int)sizeof out->name - 1 && name[i]; i++)
      out->name[i] = name[i];
    out->name[i] = '\0';
    out->is_dir = is_dir;
    out->size = size;
  }
  SC_RET(r) = (uint32_t)got;
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
  if (!user_out_range(SC_ARG1(r), sizeof *out)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  out = (struct vinfo *)SC_ARG1(r);
  struct blk_devinfo devs[8];
  int n = blk_dev_list(devs, 8), idx = (int)SC_ARG0(r);
  if (idx < 0 || idx >= n) {
    SC_RET(r) = 0;
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
  SC_RET(r) = 1;
}

/* Mounting exposes whole devices (FAT and NTFS carry no Unix modes) and unmounting pulls them from other users. */
static bool may_mount(void) { return sched_uid() == 0 || users_sudo_valid(sched_uid()); }

void syscall_fs_mount(struct registers *r)
{
  char dev[16], point[48];
  if (!may_mount() || copyin_str(SC_ARG0(r), dev, sizeof dev) != 0 ||
      copyin_str(SC_ARG1(r), point, sizeof point) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  irq_enable();
  SC_RET(r) = (uint32_t)fs_mount(dev, point);
}

void syscall_fs_umount(struct registers *r)
{
  char point[48];
  if (!may_mount() || copyin_str(SC_ARG0(r), point, sizeof point) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)fs_unmount(point);
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
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0 || !user_out_range(SC_ARG1(r), sizeof *out)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, path, sizeof path) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, path, 0))
    return;
  struct fs_stat_info st;
  int rc = fs_stat_ex(path, &st);
  if (rc == 0) {
    out = (struct st_out *)SC_ARG1(r);
    out->is_dir = st.is_dir;
    out->size = st.size;
    out->mtime = st.mtime;
    out->mode = st.mode;
    out->uid = st.uid;
    out->gid = st.gid;
  }
  SC_RET(r) = (uint32_t)rc;
}

void syscall_fs_chmod(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  struct fs_stat_info st;
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, 0))
    return;
  if (fs_stat_ex(abs, &st) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (!perm_may_chmod(st.uid, sched_uid())) {
    SC_RET(r) = (uint32_t)-PERM_EACCES;
    return;
  }
  SC_RET(r) = (uint32_t)fs_chmod(abs, (uint32_t)SC_ARG1(r));
}

/* uid/gid of -1 leave that field unchanged. */
void syscall_fs_chown(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  struct fs_stat_info st;
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (access_denied(r, abs, 0))
    return;
  if (fs_stat_ex(abs, &st) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint32_t uid = (uint32_t)SC_ARG1(r), gid = (uint32_t)SC_ARG2(r); /* -1 keeps a field */
  if (uid == (uint32_t)-1)
    uid = st.uid;
  if (gid == (uint32_t)-1)
    gid = st.gid;
  if (!perm_may_chown(st.uid, sched_uid(), sched_gid(), uid, gid)) {
    SC_RET(r) = (uint32_t)-PERM_EACCES;
    return;
  }
  SC_RET(r) = (uint32_t)fs_chown(abs, uid, gid);
}

/* A mask of -1 only queries. Returns the previous mask. */
void syscall_fs_umask(struct registers *r)
{
  uint32_t mask = (uint32_t)SC_ARG0(r); /* -1 queries */
  SC_RET(r) = mask == (uint32_t)-1 ? sched_umask() : sched_set_umask(mask);
}

void syscall_fs_access(struct registers *r)
{
  char name[SYS_PATH_MAX], abs[SYS_PATH_MAX];
  if (copyin_str(SC_ARG0(r), name, sizeof name) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (join_cwd(name, abs, sizeof abs) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)fs_access(abs, (int)(SC_ARG1(r) & 7u));
}

void syscall_fs_statfs(struct registers *r)
{
  struct sfs_out {
    uint64_t total;
    uint64_t free;
  } *out;
  if (!user_out_range(SC_ARG1(r), sizeof *out)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint64_t total = (uint64_t)blk_sectors() * 512;
  uint64_t used = fs_used_bytes();
  out = (struct sfs_out *)SC_ARG1(r);
  out->total = total;
  out->free = total > used ? total - used : 0;
  SC_RET(r) = 0;
}

void syscall_fs_uname(struct registers *r)
{
  struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65];
  } *out;
  if (!user_out_range(SC_ARG0(r), sizeof *out)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  out = (struct utsname *)SC_ARG0(r);
  memset(out, 0, sizeof *out);
  strlcpy(out->sysname, "OwOS", sizeof out->sysname);
  strlcpy(out->release, OWOS_VERSION, sizeof out->release);
  strlcpy(out->version, OWOS_VERSION, sizeof out->version);
  strlcpy(out->machine, CPU_MACHINE, sizeof out->machine);
  SC_RET(r) = 0;
}

/* Installing erases a whole disk, so it takes the same rights as mounting one. */
void syscall_fs_install(struct registers *r)
{
  if (!may_mount()) {
    SC_RET(r) = SC_ARG0(r) == INSTALL_START ? (uint32_t)-INSTALL_DENIED : (uint32_t)-1;
    return;
  }
  switch (SC_ARG0(r)) {
  case INSTALL_LIST: {
    struct install_disk d;
    int got = osinstall_disk((int)SC_ARG1(r), &d);
    SC_RET(r) = got && (!user_out_range(SC_ARG2(r), sizeof d) || copyout(SC_ARG2(r), &d, sizeof d) != 0)
                    ? (uint32_t)-1
                    : (uint32_t)got;
    return;
  }
  case INSTALL_START: {
    char dev[16];
    SC_RET(r) =
        copyin_str(SC_ARG1(r), dev, sizeof dev) != 0 ? (uint32_t)-INSTALL_NO_DEVICE : (uint32_t)-osinstall_start(dev);
    return;
  }
  case INSTALL_STEP: {
    struct install_progress p;
    int rc = osinstall_step(&p);
    SC_RET(r) =
        !user_out_range(SC_ARG1(r), sizeof p) || copyout(SC_ARG1(r), &p, sizeof p) != 0 ? (uint32_t)-1 : (uint32_t)rc;
    return;
  }
  default:
    SC_RET(r) = (uint32_t)-1;
  }
}

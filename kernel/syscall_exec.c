#include "syscall_exec.h"
#include "fs.h"
#include "heap.h"
#include "idt.h"
#include "loader.h"
#include "paging.h"
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

#include "syscall_shm.h"
#include "idt.h"
#include "shm.h"
#include "syscall_nr.h"
#include <stdint.h>

void syscall_shm(struct registers *r)
{
  int ret;
  switch (r->ebx) {
  case SHM_CREATE:
    ret = shm_create(r->ecx);
    break;
  case SHM_GRANT:
    ret = shm_grant((int)r->ecx, r->edx);
    break;
  case SHM_MAP:
    ret = shm_map((int)r->ecx, r->edx);
    break;
  case SHM_UNMAP:
    ret = shm_unmap(r->ecx);
    break;
  case SHM_RELEASE:
    ret = shm_release((int)r->ecx);
    break;
  case SHM_SIZE:
    ret = shm_size((int)r->ecx);
    break;
  default:
    ret = -1;
  }
  r->eax = (uint32_t)ret;
}

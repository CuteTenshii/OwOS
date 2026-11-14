#include "syscall_shm.h"
#include "regs.h"
#include "shm.h"
#include "syscall_nr.h"
#include <stdint.h>

void syscall_shm(struct registers *r)
{
  int ret;
  switch ((uint32_t)SC_ARG0(r)) {
  case SHM_CREATE:
    ret = shm_create((uint32_t)SC_ARG1(r));
    break;
  case SHM_GRANT:
    ret = shm_grant((int)SC_ARG1(r), (int)SC_ARG2(r));
    break;
  case SHM_MAP:
    ret = shm_map((int)SC_ARG1(r), SC_ARG2(r));
    break;
  case SHM_UNMAP:
    ret = shm_unmap(SC_ARG1(r));
    break;
  case SHM_RELEASE:
    ret = shm_release((int)SC_ARG1(r));
    break;
  case SHM_SIZE:
    ret = shm_size((int)SC_ARG1(r));
    break;
  default:
    ret = -1;
  }
  SC_RET(r) = (uint32_t)ret;
}

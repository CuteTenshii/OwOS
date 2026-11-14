/* syscall_env.c - SYS_ENV: read and edit the calling process's environment. */

#include "syscall_env.h"
#include "env.h"
#include "heap.h"
#include "kstring.h"
#include "regs.h"
#include "sched.h"
#include "syscall_nr.h"
#include "uaccess.h"

/* Copy `len` bytes at user address `src` into a new NUL-terminated kernel string; NULL on failure */
static char *copy_string(uaddr_t src, uint32_t len)
{
  if (!user_range(src, len))
    return 0;
  char *s = kmalloc(len + 1);
  if (!s)
    return 0;
  if (len && copyin(s, src, len) != 0) {
    kfree(s);
    return 0;
  }
  s[len] = '\0';
  return s;
}

/* Hand `text` to the caller's buffer (truncated to buf_len, always terminated when it holds anything); return
 * the untruncated length. */
static int give(const struct env_req *q, const char *text)
{
  uint32_t len = (uint32_t)strlen(text);
  if (q->buf_len && user_out_range(q->buf, q->buf_len)) {
    uint32_t n = len < q->buf_len ? len : q->buf_len - 1;
    char *tmp = kmalloc(n + 1);
    if (tmp) {
      memcpy(tmp, text, n);
      tmp[n] = '\0';
      copyout(q->buf, tmp, n + 1);
      kfree(tmp);
    }
  }
  return (int)len;
}

void syscall_env(struct registers *r)
{
  struct env_req q;
  struct env *e = sched_env();
  SC_RET(r) = (uint32_t)-1;
  if (!user_range(SC_ARG0(r), sizeof q) || copyin(&q, SC_ARG0(r), sizeof q) != 0)
    return;
  if (q.cmd == ENV_CLEAR) {
    env_free(e);
    SC_RET(r) = 0;
    return;
  }
  if (q.cmd == ENV_ENTRY) {
    const char *entry = env_entry(e, q.index);
    if (entry)
      SC_RET(r) = (uint32_t)give(&q, entry);
    return;
  }
  char *name = copy_string(q.name, q.name_len);
  if (!name)
    return;
  if (q.cmd == ENV_GET) {
    const char *v = env_get(e, name);
    if (v)
      SC_RET(r) = (uint32_t)give(&q, v);
  } else if (q.cmd == ENV_SET) {
    char *value = copy_string(q.value, q.value_len);
    if (value && env_set(e, name, value, q.index != 0) == 0)
      SC_RET(r) = 0;
    kfree(value);
  } else if (q.cmd == ENV_UNSET) {
    env_unset(e, name);
    SC_RET(r) = 0;
  }
  kfree(name);
}

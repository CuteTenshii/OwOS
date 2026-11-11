/* tmpfs.c: the volatile filesystem mounted at /tmp, a tree of nodes in the kernel heap (see tmpfs.h). */

#include "tmpfs.h"
#include "console.h"
#include "fsmigrate.h"
#include "heap.h"
#include "kstring.h"
#include "perm.h"
#include "sched.h"
#include "wallclock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* fs.c sizes every name buffer it hands to enum_dir with the owofs name length. */
#define TMPFS_NAME_LEN FSM_NAME_LEN

struct tnode {
  char name[TMPFS_NAME_LEN];
  bool is_dir;
  uint32_t mode, uid, gid;
  uint32_t size;
  uint32_t mtime; /* Unix seconds */
  uint8_t *data;
  struct tnode *parent, *child, *next;
};

struct tmpfs {
  struct tnode *root;
  size_t used; /* node headers plus file data */
  size_t cap;
};

static bool s_eq(const char *a, const char *b)
{
  int i = 0;
  for (; a[i] && b[i]; i++)
    if (a[i] != b[i])
      return false;
  return a[i] == b[i];
}

static void s_copy(char *dst, const char *src, int cap)
{
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

static uint32_t now(void)
{
  struct timeval64 tv;
  wallclock_get(&tv);
  return tv.tv_sec > 0 ? (uint32_t)tv.tv_sec : 0;
}

/* Whether `add` more bytes fit once `release` bytes held by the replaced object are returned. */
static bool fits(const struct tmpfs *fs, size_t add, size_t release) { return add <= fs->cap - fs->used + release; }

static struct tnode *child_by_name(struct tnode *dir, const char *name)
{
  for (struct tnode *c = dir->child; c; c = c->next)
    if (s_eq(c->name, name))
      return c;
  return NULL;
}

/* Copy the next path component into comp and advance *p past it. Returns its length, 0 at the end of the path,
 * or -1 when it does not fit in a name. */
static int next_component(const char **p, char *comp)
{
  while (**p == '/')
    (*p)++;
  int n = 0;
  while ((*p)[n] && (*p)[n] != '/')
    n++;
  if (n >= TMPFS_NAME_LEN)
    return -1;
  memcpy(comp, *p, (uint32_t)n);
  comp[n] = '\0';
  *p += n;
  return n;
}

static struct tnode *resolve(struct tmpfs *fs, const char *path)
{
  struct tnode *cur = fs->root;
  char comp[TMPFS_NAME_LEN];
  for (;;) {
    int n = next_component(&path, comp);
    if (n < 0)
      return NULL;
    if (n == 0)
      return cur;
    if (s_eq(comp, "."))
      continue;
    if (s_eq(comp, "..")) {
      if (cur->parent)
        cur = cur->parent;
      continue;
    }
    cur = cur->is_dir ? child_by_name(cur, comp) : NULL;
    if (!cur)
      return NULL;
  }
}

/* Resolve every component but the last to a directory and copy the last into leaf. leaf is "" when the path
 * names a directory itself ("/", ".", ".."), which no create, mkdir or rename target may be. */
static struct tnode *resolve_parent(struct tmpfs *fs, const char *path, char *leaf)
{
  struct tnode *cur = fs->root;
  char comp[TMPFS_NAME_LEN];
  leaf[0] = '\0';
  for (;;) {
    int n = next_component(&path, comp);
    if (n < 0)
      return NULL;
    if (n == 0)
      return cur;
    while (*path == '/')
      path++;
    bool last = *path == '\0';
    if (s_eq(comp, ".") || s_eq(comp, "..")) {
      if (comp[1] == '.' && cur->parent)
        cur = cur->parent;
      if (last)
        return cur;
      continue;
    }
    if (last) {
      s_copy(leaf, comp, TMPFS_NAME_LEN);
      return cur;
    }
    cur = child_by_name(cur, comp);
    if (!cur || !cur->is_dir)
      return NULL;
  }
}

static struct tnode *node_new(struct tmpfs *fs, const char *name, bool is_dir)
{
  if (!fits(fs, sizeof(struct tnode), 0))
    return NULL;
  struct tnode *n = kmalloc(sizeof *n);
  if (!n)
    return NULL;
  fs->used += sizeof *n;
  s_copy(n->name, name, TMPFS_NAME_LEN);
  n->is_dir = is_dir;
  n->mode = perm_apply_umask(is_dir ? 0777u : 0666u, sched_umask());
  n->uid = sched_uid();
  n->gid = sched_gid();
  n->size = 0;
  n->mtime = now();
  n->data = NULL;
  n->parent = n->child = n->next = NULL;
  return n;
}

static void node_free(struct tmpfs *fs, struct tnode *n)
{
  struct tnode *c = n->child;
  while (c) {
    struct tnode *next = c->next;
    node_free(fs, c);
    c = next;
  }
  kfree(n->data);
  fs->used -= sizeof *n + n->size;
  kfree(n);
}

static void link_child(struct tnode *dir, struct tnode *child)
{
  child->parent = dir;
  child->next = dir->child;
  dir->child = child;
  dir->mtime = now();
}

static void unlink_child(struct tnode *child)
{
  struct tnode *dir = child->parent;
  struct tnode **pp = &dir->child;
  while (*pp != child)
    pp = &(*pp)->next;
  *pp = child->next;
  child->parent = child->next = NULL;
  dir->mtime = now();
}

static const char *tmpfs_label(void *inst)
{
  (void)inst;
  return "";
}

static const char *tmpfs_type(void *inst)
{
  (void)inst;
  return "tmpfs";
}

static int tmpfs_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  struct tnode *n = resolve(inst, path);
  if (!n)
    return -1;
  if (is_dir)
    *is_dir = n->is_dir;
  if (size)
    *size = n->size;
  return 0;
}

static int tmpfs_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  struct tnode *n = resolve(inst, path);
  if (!n || n->is_dir)
    return -1;
  if (off >= n->size)
    return 0;
  uint32_t want = n->size - off < len ? n->size - off : len;
  memcpy(buf, n->data + off, want);
  return (int)want;
}

static void write_size(uint32_t v)
{
  char digits[10];
  int n = 0;
  do
    digits[n++] = (char)('0' + v % 10);
  while ((v /= 10) && n < 10);
  for (int pad = n; pad < 8; pad++)
    con_putc(' ');
  while (n)
    con_putc(digits[--n]);
}

static void list_row(const struct tnode *n)
{
  char mode[11];
  perm_mode_str(n->mode, n->is_dir, mode);
  con_write(mode);
  con_write("  ");
  if (n->is_dir)
    con_write("   <DIR>");
  else
    write_size(n->size);
  con_write("  ");
  con_write(n->name);
  if (n->is_dir)
    con_putc('/');
  con_putc('\n');
}

static int tmpfs_list(void *inst, const char *path)
{
  struct tnode *n = resolve(inst, path);
  if (!n)
    return -1;
  if (!n->is_dir) {
    list_row(n);
    return 0;
  }
  for (struct tnode *c = n->child; c; c = c->next)
    list_row(c);
  if (!n->child)
    con_write("(empty)\n");
  return 0;
}

static int tmpfs_enum(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  struct tnode *dir = resolve(inst, path);
  if (!dir || !dir->is_dir || idx < 0)
    return 0;
  struct tnode *c = dir->child;
  for (; c && idx > 0; idx--)
    c = c->next;
  if (!c)
    return 0;
  s_copy(name, c->name, TMPFS_NAME_LEN);
  if (is_dir)
    *is_dir = c->is_dir;
  if (size)
    *size = c->size;
  return 1;
}

static int tmpfs_create(void *inst, const char *path, const void *data, uint32_t size)
{
  struct tmpfs *fs = inst;
  char leaf[TMPFS_NAME_LEN];
  struct tnode *dir = resolve_parent(fs, path, leaf);
  if (!dir || !leaf[0] || (size && !data))
    return -1;
  struct tnode *old = child_by_name(dir, leaf);
  if (old && old->is_dir)
    return -1;
  if (!fits(fs, size + (old ? 0 : sizeof(struct tnode)), old ? old->size : 0))
    return -1;

  /* Copy before releasing the old contents: the caller may be rewriting the file from its own buffer. */
  uint8_t *buf = NULL;
  if (size) {
    buf = kmalloc(size);
    if (!buf)
      return -1;
    memcpy(buf, data, size);
  }
  struct tnode *n = old ? old : node_new(fs, leaf, false);
  if (!n) {
    kfree(buf);
    return -1;
  }
  kfree(n->data);
  fs->used = fs->used - n->size + size;
  n->data = buf;
  n->size = size;
  n->mtime = now();
  if (!old)
    link_child(dir, n);
  return 0;
}

static int tmpfs_write_at(void *inst, const char *path, uint32_t off, const void *buf, uint32_t len)
{
  struct tmpfs *fs = inst;
  struct tnode *n = resolve(fs, path);
  if (!n || n->is_dir || len > UINT32_MAX - off || len > INT32_MAX)
    return -1;
  uint32_t end = off + len;
  if (end > n->size) {
    if (!fits(fs, end - n->size, 0))
      return -1;
    uint8_t *grown = kmalloc(end);
    if (!grown)
      return -1;
    if (n->size)
      memcpy(grown, n->data, n->size);
    memset(grown + n->size, 0, end - n->size);
    kfree(n->data);
    fs->used += end - n->size;
    n->data = grown;
    n->size = end;
  }
  if (len)
    memcpy(n->data + off, buf, len);
  n->mtime = now();
  return (int)len;
}

static int tmpfs_del(void *inst, const char *path)
{
  struct tnode *n = resolve(inst, path);
  if (!n || n->is_dir)
    return -1;
  unlink_child(n);
  node_free(inst, n);
  return 0;
}

static int tmpfs_mkdir(void *inst, const char *path)
{
  struct tmpfs *fs = inst;
  char leaf[TMPFS_NAME_LEN];
  struct tnode *dir = resolve_parent(fs, path, leaf);
  if (!dir || !leaf[0] || child_by_name(dir, leaf))
    return -1;
  struct tnode *n = node_new(fs, leaf, true);
  if (!n)
    return -1;
  link_child(dir, n);
  return 0;
}

static int tmpfs_rmdir(void *inst, const char *path)
{
  struct tnode *n = resolve(inst, path);
  if (!n || !n->is_dir || !n->parent || n->child)
    return -1;
  unlink_child(n);
  node_free(inst, n);
  return 0;
}

/* Same rules as owofs: a file at the target is replaced, a directory there or a move into its own subtree fails. */
static int tmpfs_rename(void *inst, const char *from, const char *to)
{
  struct tmpfs *fs = inst;
  struct tnode *src = resolve(fs, from);
  char leaf[TMPFS_NAME_LEN];
  struct tnode *dir = resolve_parent(fs, to, leaf);
  if (!src || !src->parent || !dir || !leaf[0])
    return -1;
  for (struct tnode *a = dir; a; a = a->parent)
    if (a == src)
      return -1;
  struct tnode *ex = child_by_name(dir, leaf);
  if (ex == src)
    return 0;
  if (ex) {
    if (ex->is_dir)
      return -1;
    unlink_child(ex);
    node_free(fs, ex);
  }
  unlink_child(src);
  s_copy(src->name, leaf, TMPFS_NAME_LEN);
  link_child(dir, src);
  src->mtime = now();
  return 0;
}

static int tmpfs_getattr(void *inst, const char *path, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
  struct tnode *n = resolve(inst, path);
  if (!n)
    return -1;
  *mode = n->mode;
  *uid = n->uid;
  *gid = n->gid;
  return 0;
}

static int tmpfs_mtime(void *inst, const char *path, uint32_t *mtime)
{
  struct tnode *n = resolve(inst, path);
  if (!n)
    return -1;
  *mtime = n->mtime;
  return 0;
}

static void tmpfs_close(void *inst)
{
  struct tmpfs *fs = inst;
  node_free(fs, fs->root);
  kfree(fs);
}

static const struct fs_ops s_tmpfs_ops = {
    .fsname = "tmpfs",
    .probe = NULL,
    .close = tmpfs_close,
    .label = tmpfs_label,
    .type = tmpfs_type,
    .list = tmpfs_list,
    .stat = tmpfs_stat,
    .read_at = tmpfs_read_at,
    .enum_dir = tmpfs_enum,
    .create = tmpfs_create,
    .del = tmpfs_del,
    .mkdir = tmpfs_mkdir,
    .rmdir = tmpfs_rmdir,
    .getattr = tmpfs_getattr,
    .rename = tmpfs_rename,
    .write_at = tmpfs_write_at,
    .mtime = tmpfs_mtime,
};

const struct fs_ops *tmpfs_ops(void) { return &s_tmpfs_ops; }

void *tmpfs_new(void)
{
  struct tmpfs *fs = kmalloc(sizeof *fs);
  if (!fs)
    return NULL;
  fs->used = 0;
  fs->cap = heap_free_bytes() / 4;
  fs->root = node_new(fs, "", true);
  if (!fs->root) {
    kfree(fs);
    return NULL;
  }
  fs->root->mode = 01777;
  fs->root->uid = fs->root->gid = 0;
  return fs;
}

/* hid_desc.c: HID report descriptor parser, report field extraction and report decoding. */

#include "hid_desc.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#define hid_alloc malloc
#define hid_free free
#else
#include "heap.h"
#include "kstring.h"
#define hid_alloc kmalloc
#define hid_free kfree
#endif

/* Guards against hostile descriptors, far above anything real devices declare. */
#define HID_MAX_COUNT 4096
#define HID_MAX_FIELDS 16384
#define HID_MAX_USAGES 65536

const uint8_t hid_boot_kbd_desc[63] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

const uint8_t hid_boot_mouse_desc[50] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15,
    0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};

/* Grow *p (element size elem) so it holds at least need elements; false on out of memory. */
static bool grow(void **p, size_t *cap, size_t need, size_t elem)
{
  if (need <= *cap)
    return true;
  size_t ncap = *cap ? *cap * 2 : 8;
  while (ncap < need)
    ncap *= 2;
  void *n = hid_alloc(ncap * elem);
  if (!n)
    return false;
  if (*p) {
    memcpy(n, *p, *cap * elem);
    hid_free(*p);
  }
  *p = n;
  *cap = ncap;
  return true;
}

struct gstate {
  uint32_t page;
  int32_t lmin, lmax, pmin, pmax;
  uint32_t size, count;
  uint8_t id;
};

struct col {
  uint32_t usage;
  uint16_t serial;
};

struct pstate {
  struct gstate g;
  struct gstate *stack;
  size_t nstack, cap_stack;
  struct col *cols;
  size_t ncols, cap_cols;
  uint32_t *usages;
  size_t nusages, cap_usages;
  uint32_t umin;
  bool have_min;
  bool in_delim, delim_taken;
  uint16_t serial;
};

static bool add_usage(struct pstate *s, uint32_t u)
{
  if (s->in_delim) {
    if (s->delim_taken)
      return true;
    s->delim_taken = true;
  }
  if (s->nusages >= HID_MAX_USAGES || !grow((void **)&s->usages, &s->cap_usages, s->nusages + 1, sizeof *s->usages))
    return false;
  s->usages[s->nusages++] = u;
  return true;
}

static void clear_locals(struct pstate *s)
{
  s->nusages = 0;
  s->have_min = false;
  s->in_delim = s->delim_taken = false;
}

static struct hid_report_info *report_for(struct hid_desc *d, uint8_t kind, uint8_t id)
{
  for (size_t i = 0; i < d->nreports; i++)
    if (d->reports[i].kind == kind && d->reports[i].id == id)
      return &d->reports[i];
  if (!grow((void **)&d->reports, &d->cap_reports, d->nreports + 1, sizeof *d->reports))
    return NULL;
  struct hid_report_info *r = &d->reports[d->nreports++];
  r->kind = kind;
  r->id = id;
  r->bits = 0;
  return r;
}

static bool add_field(struct hid_desc *d, const struct hid_field *f)
{
  if (d->nfields >= HID_MAX_FIELDS || !grow((void **)&d->fields, &d->cap_fields, d->nfields + 1, sizeof *d->fields))
    return false;
  d->fields[d->nfields++] = *f;
  return true;
}

/* Handle one Input/Output/Feature item. */
static bool main_item(struct pstate *s, struct hid_desc *d, uint8_t kind, uint32_t flags)
{
  struct gstate *g = &s->g;
  if (g->count > HID_MAX_COUNT)
    return false;
  struct hid_report_info *r = report_for(d, kind, g->id);
  if (!r)
    return false;

  struct hid_field f = {0};
  f.kind = kind;
  f.report_id = g->id;
  f.flags = flags;
  f.lmin = g->lmin;
  f.lmax = g->lmax;
  f.pmin = g->pmin;
  f.pmax = g->pmax;
  f.size = (uint16_t)g->size;
  f.col = s->ncols ? s->cols[s->ncols - 1].serial : 0;
  f.col_usage = s->ncols ? s->cols[s->ncols - 1].usage : 0;
  f.app = s->ncols ? s->cols[0].usage : 0;

  if (!(flags & HID_F_CONST) && g->size >= 1 && g->size <= 32) {
    if (flags & HID_F_VAR) {
      for (uint32_t i = 0; i < g->count; i++) {
        f.bit_off = r->bits + i * g->size;
        f.count = 1;
        if (s->nusages)
          f.usage = s->usages[i < s->nusages ? i : s->nusages - 1];
        else
          f.usage = HID_USAGE(g->page, 0);
        f.usage_max = f.usage;
        if (!add_field(d, &f))
          return false;
      }
    } else if (g->count) {
      f.bit_off = r->bits;
      f.count = (uint16_t)g->count;
      f.usage = s->nusages ? s->usages[0] : HID_USAGE(g->page, 0);
      f.usage_max = s->nusages ? s->usages[s->nusages - 1] : f.usage;
      if (!add_field(d, &f))
        return false;
    }
  }
  r->bits += g->size * g->count;
  clear_locals(s);
  return true;
}

static uint32_t item_data(const uint8_t *p, unsigned size)
{
  uint32_t v = 0;
  for (unsigned i = 0; i < size; i++)
    v |= (uint32_t)p[i] << (8 * i);
  return v;
}

static int32_t sign_extend(uint32_t v, unsigned size)
{
  if (size == 1)
    return (int8_t)v;
  if (size == 2)
    return (int16_t)v;
  return (int32_t)v;
}

static void parse_free(struct pstate *s)
{
  hid_free(s->stack);
  hid_free(s->cols);
  hid_free(s->usages);
}

int hid_parse(const uint8_t *p, size_t n, struct hid_desc *out)
{
  struct pstate s = {0};
  memset(out, 0, sizeof *out);
  size_t mains = 0;
  bool ok = true;

  for (size_t i = 0; ok && i < n;) {
    uint8_t prefix = p[i];
    if (prefix == 0xFE) {
      if (i + 3 > n) {
        ok = false;
        break;
      }
      size_t skip = 3 + (size_t)p[i + 1];
      if (i + skip > n) {
        ok = false;
        break;
      }
      i += skip;
      continue;
    }
    unsigned size = prefix & 3;
    if (size == 3)
      size = 4;
    unsigned type = (prefix >> 2) & 3;
    unsigned tag = prefix >> 4;
    if (i + 1 + size > n) {
      ok = false;
      break;
    }
    uint32_t raw = item_data(p + i + 1, size);
    int32_t sval = sign_extend(raw, size);
    i += 1 + size;

    if (type == 0) { /* main */
      mains++;
      switch (tag) {
      case 0x8:
        ok = main_item(&s, out, HID_INPUT, raw);
        break;
      case 0x9:
        ok = main_item(&s, out, HID_OUTPUT, raw);
        break;
      case 0xB:
        ok = main_item(&s, out, HID_FEATURE, raw);
        break;
      case 0xA: {
        uint32_t usage = s.nusages ? s.usages[0] : 0;
        if (!grow((void **)&s.cols, &s.cap_cols, s.ncols + 1, sizeof *s.cols)) {
          ok = false;
          break;
        }
        s.cols[s.ncols].usage = usage;
        s.cols[s.ncols].serial = ++s.serial;
        s.ncols++;
        clear_locals(&s);
        break;
      }
      case 0xC:
        if (s.ncols)
          s.ncols--;
        clear_locals(&s);
        break;
      }
    } else if (type == 1) { /* global */
      switch (tag) {
      case 0x0:
        s.g.page = raw & 0xFFFF;
        break;
      case 0x1:
        s.g.lmin = sval;
        break;
      case 0x2:
        s.g.lmax = s.g.lmin < 0 ? sval : (int32_t)raw;
        break;
      case 0x3:
        s.g.pmin = sval;
        break;
      case 0x4:
        s.g.pmax = s.g.pmin < 0 ? sval : (int32_t)raw;
        break;
      case 0x7:
        s.g.size = raw;
        break;
      case 0x8:
        s.g.id = (uint8_t)raw;
        out->has_ids = true;
        break;
      case 0x9:
        s.g.count = raw;
        break;
      case 0xA:
        if (!grow((void **)&s.stack, &s.cap_stack, s.nstack + 1, sizeof *s.stack))
          ok = false;
        else
          s.stack[s.nstack++] = s.g;
        break;
      case 0xB:
        if (s.nstack)
          s.g = s.stack[--s.nstack];
        break;
      }
    } else if (type == 2) { /* local */
      uint32_t full = size == 4 ? raw : (s.g.page << 16) | (raw & 0xFFFF);
      switch (tag) {
      case 0x0:
        ok = add_usage(&s, full);
        break;
      case 0x1:
        s.umin = full;
        s.have_min = true;
        break;
      case 0x2:
        if (s.have_min && full >= s.umin && full - s.umin < HID_MAX_USAGES) {
          for (uint32_t u = s.umin; ok && u <= full; u++)
            ok = add_usage(&s, u);
        }
        s.have_min = false;
        break;
      case 0xA:
        s.in_delim = raw == 1;
        s.delim_taken = false;
        break;
      }
    }
  }

  parse_free(&s);
  if (!ok || mains == 0) {
    hid_desc_free(out);
    return -1;
  }
  return 0;
}

void hid_desc_free(struct hid_desc *d)
{
  hid_free(d->fields);
  hid_free(d->reports);
  memset(d, 0, sizeof *d);
}

uint32_t hid_report_bytes(const struct hid_desc *d, uint8_t kind, uint8_t id)
{
  for (size_t i = 0; i < d->nreports; i++)
    if (d->reports[i].kind == kind && d->reports[i].id == id)
      return (d->reports[i].bits + 7) / 8;
  return 0;
}

int32_t hid_get_bits(const uint8_t *p, size_t len, uint32_t bit_off, unsigned size, bool is_signed)
{
  uint32_t v = 0;
  for (unsigned b = 0; b < size; b++) {
    uint32_t pos = bit_off + b;
    size_t byte = pos >> 3;
    if (byte < len && (p[byte] >> (pos & 7)) & 1)
      v |= 1u << b;
  }
  if (is_signed && size < 32 && (v & (1u << (size - 1))))
    v |= ~0u << size;
  return (int32_t)v;
}

void hid_set_bits(uint8_t *p, size_t len, uint32_t bit_off, unsigned size, uint32_t v)
{
  for (unsigned b = 0; b < size; b++) {
    uint32_t pos = bit_off + b;
    size_t byte = pos >> 3;
    if (byte >= len)
      continue;
    if ((v >> b) & 1)
      p[byte] |= (uint8_t)(1u << (pos & 7));
    else
      p[byte] &= (uint8_t)~(1u << (pos & 7));
  }
}

static bool is_pad_app(uint32_t app)
{
  return app == HID_USAGE(HID_PAGE_DESKTOP, 4) || app == HID_USAGE(HID_PAGE_DESKTOP, 5);
}

static bool is_finger(uint32_t col_usage) { return col_usage == HID_USAGE(HID_PAGE_DIGITIZER, 0x22); }

static bool is_special_page(uint32_t usage)
{
  uint32_t page = usage >> 16, id = usage & 0xFFFF;
  return (page == HID_PAGE_CONSUMER && id != HID_AC_PAN) || page == HID_PAGE_TELEPHONY ||
         (page == HID_PAGE_DESKTOP && id >= 0x80 && id <= 0x83);
}

uint32_t hid_roles(const struct hid_desc *d)
{
  uint32_t roles = 0;
  bool ax = false, ay = false, rel = false;
  for (size_t i = 0; i < d->nfields; i++) {
    const struct hid_field *f = &d->fields[i];
    uint32_t page = f->usage >> 16, id = f->usage & 0xFFFF;
    if (f->kind != HID_INPUT)
      continue;
    if (page == HID_PAGE_KEYBOARD)
      roles |= HID_ROLE_KEYBOARD;
    else if (is_special_page(f->usage) || is_special_page(f->usage_max))
      roles |= HID_ROLE_CONSUMER;
    if (is_pad_app(f->app)) {
      if (page == HID_PAGE_BUTTON || (page == HID_PAGE_DESKTOP && id >= 0x30 && id <= 0x39))
        roles |= HID_ROLE_GAMEPAD;
    } else if (page == HID_PAGE_DESKTOP && (id == 0x30 || id == 0x31)) {
      if (id == 0x30)
        ax = true;
      else
        ay = true;
      if (f->flags & HID_F_REL)
        rel = true;
    } else if (page == HID_PAGE_DIGITIZER && id == 0x42) {
      roles |= HID_ROLE_TABLET;
    }
  }
  if (ax && ay)
    roles |= rel ? HID_ROLE_MOUSE : HID_ROLE_TABLET;
  return roles;
}

int hid_decoded_init(struct hid_decoded *d, const struct hid_desc *desc)
{
  memset(d, 0, sizeof *d);
  uint32_t kn = 0, cn = 0;
  for (size_t i = 0; i < desc->nfields; i++) {
    const struct hid_field *f = &desc->fields[i];
    if (f->kind != HID_INPUT)
      continue;
    uint32_t page = f->usage >> 16;
    uint32_t n = (f->flags & HID_F_VAR) ? 1 : f->count;
    if (page == HID_PAGE_KEYBOARD)
      kn += n;
    else if (is_special_page(f->usage) || is_special_page(f->usage_max))
      cn += n;
    if (!d->touch_col_set && is_finger(f->col_usage)) {
      d->touch_col = f->col;
      d->touch_col_set = true;
    }
  }
  d->keys_cap = kn;
  d->cons_cap = cn;
  d->keys = kn ? hid_alloc(kn * sizeof *d->keys) : NULL;
  d->cons = cn ? hid_alloc(cn * sizeof *d->cons) : NULL;
  if ((kn && !d->keys) || (cn && !d->cons)) {
    hid_decoded_free(d);
    return -1;
  }
  return 0;
}

void hid_decoded_free(struct hid_decoded *d)
{
  hid_free(d->keys);
  hid_free(d->cons);
  memset(d, 0, sizeof *d);
}

static void add_key(struct hid_decoded *d, uint16_t usage)
{
  if (d->nkeys < d->keys_cap)
    d->keys[d->nkeys++] = usage;
}

static void add_cons(struct hid_decoded *d, uint32_t usage)
{
  if (d->ncons < d->cons_cap)
    d->cons[d->ncons++] = usage;
}

static uint8_t hat_dir(int32_t v, const struct hid_field *f)
{
  int32_t n = f->lmax - f->lmin + 1;
  if (v < f->lmin || v > f->lmax)
    return 0;
  if (n == 8)
    return (uint8_t)(v - f->lmin + 1);
  if (n == 4)
    return (uint8_t)((v - f->lmin) * 2 + 1);
  return 0;
}

static void decode_array(struct hid_decoded *d, const struct hid_field *f, const uint8_t *p, size_t len)
{
  uint32_t page = f->usage >> 16, base = f->usage & 0xFFFF, last = f->usage_max & 0xFFFF;
  bool kbd = page == HID_PAGE_KEYBOARD;
  uint32_t rollover = 0;
  if (kbd)
    d->has_kbd = true;
  else if (is_special_page(f->usage))
    d->has_cons = true;
  else
    return;
  for (uint32_t i = 0; i < f->count; i++) {
    int32_t v = hid_get_bits(p, len, f->bit_off + i * f->size, f->size, f->lmin < 0);
    if (v < f->lmin || v > f->lmax)
      continue;
    uint32_t id = base + (uint32_t)(v - f->lmin);
    if (id > last || id == 0)
      continue;
    if (kbd) {
      if (id == 1)
        rollover++;
      else if (id >= 4 && id <= 0xFF)
        add_key(d, (uint16_t)id);
    } else {
      add_cons(d, (page << 16) | id);
    }
  }
  if (kbd && rollover == f->count)
    d->phantom = true;
}

static void decode_var(struct hid_decoded *d, const struct hid_field *f, const uint8_t *p, size_t len)
{
  uint32_t page = f->usage >> 16, id = f->usage & 0xFFFF;
  int32_t v = hid_get_bits(p, len, f->bit_off, f->size, f->lmin < 0);
  bool rel = f->flags & HID_F_REL;
  bool pad = is_pad_app(f->app);

  switch (page) {
  case HID_PAGE_KEYBOARD:
    if (id >= 0xE0 && id <= 0xE7) {
      d->has_kbd = true;
      if (v)
        d->mods |= (uint8_t)(1u << (id - 0xE0));
    } else if (id >= 4 && id <= 0xFF) {
      d->has_kbd = true;
      if (v)
        add_key(d, (uint16_t)id);
    }
    break;
  case HID_PAGE_BUTTON:
    if (id < 1 || id > 32)
      break;
    if (pad) {
      d->has_pad = true;
      if (v)
        d->pad_buttons |= 1u << (id - 1);
    } else {
      d->has_buttons = true;
      if (v)
        d->buttons |= 1u << (id - 1);
    }
    break;
  case HID_PAGE_DESKTOP:
    if (id >= 0x30 && id <= 0x37 && pad) {
      d->has_pad = true;
      d->axes[id - 0x30] = v;
      d->axes_min[id - 0x30] = f->lmin;
      d->axes_max[id - 0x30] = f->lmax;
    } else if (id == 0x39 && pad) {
      d->has_pad = true;
      d->hat = hat_dir(v, f);
    } else if (id == 0x30 || id == 0x31) {
      if (rel) {
        d->has_rel = true;
        if (id == 0x30)
          d->dx += v;
        else
          d->dy += v;
      } else {
        d->has_abs = true;
        if (id == 0x30) {
          d->ax = v;
          d->ax_min = f->lmin;
          d->ax_max = f->lmax;
        } else {
          d->ay = v;
          d->ay_min = f->lmin;
          d->ay_max = f->lmax;
        }
      }
    } else if (id == 0x38 && rel && !pad) {
      d->has_rel = true;
      d->wheel += v;
    } else if (id >= 0x80 && id <= 0x83) {
      d->has_cons = true;
      if (v)
        add_cons(d, f->usage);
    }
    break;
  case HID_PAGE_CONSUMER:
    if (id == HID_AC_PAN && rel) {
      d->has_rel = true;
      d->pan += v;
    } else {
      d->has_cons = true;
      if (v)
        add_cons(d, f->usage);
    }
    break;
  case HID_PAGE_TELEPHONY:
    d->has_cons = true;
    if (v)
      add_cons(d, f->usage);
    break;
  case HID_PAGE_DIGITIZER:
    if (id == 0x42 || id == 0x45) {
      d->has_touch = true;
      if (v)
        d->tip = true;
    } else if (id == 0x32) {
      d->has_range = true;
      d->in_range = v != 0;
    } else if (id == 0x44 || id == 0x5A) {
      d->has_buttons = true;
      if (v)
        d->buttons |= id == 0x44 ? 2 : 4;
    }
    break;
  }
}

void hid_decode(const struct hid_desc *desc, const uint8_t *data, size_t len, struct hid_decoded *d)
{
  d->valid = false;
  d->has_kbd = d->has_cons = d->has_rel = d->has_abs = d->has_buttons = d->has_touch = d->has_pad = false;
  d->phantom = d->tip = d->in_range = d->has_range = false;
  d->mods = 0;
  d->nkeys = d->ncons = 0;
  d->buttons = d->pad_buttons = 0;
  d->dx = d->dy = d->wheel = d->pan = 0;
  d->hat = 0;
  d->report_id = 0;

  const uint8_t *p = data;
  if (desc->has_ids) {
    if (!len)
      return;
    d->report_id = data[0];
    p++;
    len--;
  }
  for (size_t i = 0; i < desc->nfields; i++) {
    const struct hid_field *f = &desc->fields[i];
    if (f->kind != HID_INPUT || f->report_id != d->report_id)
      continue;
    d->valid = true;
    if (d->touch_col_set && is_finger(f->col_usage) && f->col != d->touch_col)
      continue;
    if (f->flags & HID_F_CONST)
      continue;
    if (f->flags & HID_F_VAR)
      decode_var(d, f, p, len);
    else
      decode_array(d, f, p, len);
  }
}

int hid_pack_leds(const struct hid_desc *desc, uint8_t leds, uint8_t *buf, size_t len, uint8_t *id_out)
{
  int id = -1;
  for (size_t i = 0; i < desc->nfields; i++) {
    const struct hid_field *f = &desc->fields[i];
    if (f->kind != HID_OUTPUT || (f->usage >> 16) != HID_PAGE_LED || !(f->flags & HID_F_VAR))
      continue;
    uint32_t n = f->usage & 0xFFFF;
    if (n < 1 || n > 8)
      continue;
    if (id < 0) {
      id = f->report_id;
      memset(buf, 0, len);
    } else if (id != f->report_id) {
      continue;
    }
    hid_set_bits(buf, len, f->bit_off, f->size, (leds >> (n - 1)) & 1);
  }
  if (id >= 0 && id_out)
    *id_out = (uint8_t)id;
  return id;
}

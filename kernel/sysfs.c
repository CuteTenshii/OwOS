/* sysfs.c - a Linux-sysfs-style /sys hierarchy (see sysfs.h).
 *
 * A real nested tree of kernel objects and their attribute files, reached
 * through the ordinary file API. Unlike the flat /proc, every directory level
 * here is walkable (`ls /sys/class/input` -> kbd0 mouse0). Layout:
 *
 *   /sys/class/input/kbd0/layout        keyboard layout       (rw)
 *   /sys/class/input/mouse0/speed       pointer speed 1..5    (rw)
 *   /sys/class/net/eth0/{address,carrier,ipaddr,ip6addr,ip6ll,dns6} (ro)
 *   /sys/class/block/<dev>/{model,size}                       (ro)
 *   /sys/class/graphics/fb0/{width,height,refresh,monitor,internal}  (ro)
 *   /sys/class/graphics/fb0/{dpi,scale_active}  EDID DPI, applied UI scale (ro)
 *   /sys/class/graphics/fb0/{hdmi,hdmi_audio}  HDMI sink / HDMI audio support (ro)
 *   /sys/class/graphics/fb0/connected          re-probes the monitor, 1 if present (ro)
 *   /sys/class/graphics/fb0/scale              UI scale "auto"|1..4 (rw)
 *   /sys/class/graphics/fb0/{mode,modes}       screen resolution   (rw/ro,
 * DISPI) /sys/class/sound/card0/{output,outputs}    audio output select (rw/ro)
 *   /sys/class/sound/card0/{capture,capture_pcm}  microphone: start/stop + position, PCM stream (rw / ro binary)
 *   /sys/class/video/cam0/{info,control,frame}  USB camera: formats, start/stop, newest frame (ro/rw/ro binary)
 *   /sys/class/dmi/id/{product_name,sys_vendor}               (ro, SMBIOS)
 *   /sys/bus/pci/devices/<bdf>/{vendor,device,class}          (ro, one
 * dir/device) /sys/power/battery/{capacity,status}                      (ro,
 * real HW) /sys/firmware/type                  BIOS / UEFI           (ro)
 *
 * The tree is small, and the dynamic parts (which NIC/block devices exist, is a
 * battery present) depend on hardware, so it's resolved by an explicit
 * per-level path walk rather than a static node table. Classification,
 * enumeration, read and write all share the same level structure.
 */

#include "sysfs.h"
#include "audio.h"
#include "backlight.h"
#include "battery.h"
#include "blk.h"
#include "bt.h"
#include "edid.h"
#include "fb.h"
#include "firmware.h"
#include "hda.h"
#include "hid.h"
#include "keyboard.h"
#include "kstring.h"
#include "mouse.h"
#include "net.h"
#include "nic.h"
#include "pci.h"
#include "smbios.h"
#include "uvc.h"
#include <stdint.h>

/* The single NIC's sysfs name (we keep one card; nic.c is driver-agnostic). */
#define NET_IFACE "eth0"

static bool gamepad_present(void)
{
  char probe[2];
  return hid_gamepad_state(probe, sizeof probe) >= 0;
}

static int parse_percent(const char *p)
{
  int v = 0;
  if (*p < '0' || *p > '9')
    return -1;
  for (; *p >= '0' && *p <= '9'; p++)
    v = v < 1000 ? v * 10 + (*p - '0') : v;
  return v > 100 ? 100 : v;
}

/* Unsigned decimal, saturating at UINT32_MAX; -1 if it doesn't start with a digit. */
static int parse_u32(const char *p, uint32_t *out)
{
  uint32_t v = 0;
  if (*p < '0' || *p > '9')
    return -1;
  for (; *p >= '0' && *p <= '9'; p++) {
    uint32_t d = (uint32_t)(*p - '0');
    v = v > (UINT32_MAX - d) / 10 ? UINT32_MAX : v * 10 + d;
  }
  *out = v;
  return 0;
}

/* text builder */
/* Same bounded builder as procfs.c, local so the two stay independent. */
struct pb {
  char *p;
  char *end;
};
static void ps(struct pb *b, const char *s)
{
  while (s && *s && b->p < b->end)
    *b->p++ = *s++;
}
static void pc(struct pb *b, char c)
{
  if (b->p < b->end)
    *b->p++ = c;
}
static void pu(struct pb *b, uint32_t v)
{
  char t[12];
  int n = 0;
  if (!v) {
    pc(b, '0');
    return;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n)
    pc(b, t[--n]);
}
static void phex2(struct pb *b, uint8_t v)
{
  const char *h = "0123456789abcdef";
  pc(b, h[v >> 4]);
  pc(b, h[v & 0xF]);
}
static void phex4(struct pb *b, uint16_t v)
{
  phex2(b, v >> 8);
  phex2(b, v & 0xFF);
}

/* path tokeniser */
#define MAXSEG 6 /* deepest path is bus/pci/devices/<bdf>/<attr> = 5 segments */
#define SEGLEN 24

/* Split `sub` ("/class/input/mouse0/speed") into segments; returns the count.
 */
static int split(const char *sub, char seg[MAXSEG][SEGLEN])
{
  int n = 0;
  const char *p = sub ? sub : "";
  while (*p == '/')
    p++;
  while (*p && n < MAXSEG) {
    int i = 0;
    while (*p && *p != '/' && i < SEGLEN - 1)
      seg[n][i++] = *p++;
    seg[n][i] = 0;
    while (*p && *p != '/')
      p++; /* skip an over-long segment's tail */
    while (*p == '/')
      p++;
    n++;
  }
  return n;
}

static int eq(const char *a, const char *b) { return streq(a, b); }
static void cpy(char *d, const char *s, int max)
{
  int i = 0;
  for (; s[i] && i < max - 1; i++)
    d[i] = s[i];
  d[i] = 0;
}

/* ------------------------------------------------------------- block helpers
 */
/* The backing owofs device appears as "<backend>0" (ide0/ahci0/nvme0/tmpfs0);
 * removable devices keep their /dev names (usb0/cd0/...). */
static int backend_name(char *out)
{
  const char *b = blk_backend();
  if (!b || eq(b, "none")) {
    out[0] = 0;
    return 0;
  }
  int i = 0;
  for (; b[i] && i < SEGLEN - 2; i++) {
    char c = b[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[i++] = '0';
  out[i] = 0;
  return 1;
}

/* idx 0 = backing device (if any), then each removable device. 1 if it exists.
 */
static int block_name(int idx, char *out)
{
  char bk[SEGLEN];
  int have_backing = backend_name(bk);
  if (have_backing && idx == 0) {
    cpy(out, bk, SEGLEN);
    return 1;
  }
  int rel = have_backing ? idx - 1 : idx;
  struct blk_devinfo d[16];
  int nd = blk_dev_list(d, 16);
  if (rel < 0 || rel >= nd)
    return 0;
  cpy(out, d[rel].dev, SEGLEN);
  return 1;
}

/* model/size for a block device by name; size is in 512-byte sectors. */
static void block_attr(const char *name, struct pb *b, int want_size)
{
  char bk[SEGLEN];
  if (backend_name(bk) && eq(name, bk)) {
    if (want_size)
      pu(b, blk_sectors());
    else
      ps(b, blk_model());
    pc(b, '\n');
    return;
  }
  struct blk_devinfo d[16];
  int nd = blk_dev_list(d, 16);
  for (int i = 0; i < nd; i++)
    if (eq(d[i].dev, name)) {
      if (want_size)
        pu(b, 0); /* per-device capacity not probed */
      else
        ps(b, d[i].model);
      pc(b, '\n');
      return;
    }
  pc(b, '\n');
}

static bool block_exists(const char *name)
{
  char nm[SEGLEN];
  for (int i = 0; block_name(i, nm); i++)
    if (eq(nm, name))
      return true;
  return false;
}

/* ------------------------------------------------------------- PCI helpers */
/* Each PCI device is a directory named by its Linux-style BDF address
 * "domain:bus:slot.func" - domain is always 0000 here (no PCI segments). */
static void pci_bdf_str(const struct pci_dev_info *d, char *out)
{
  const char *h = "0123456789abcdef";
  out[0] = '0';
  out[1] = '0';
  out[2] = '0';
  out[3] = '0';
  out[4] = ':';
  out[5] = h[(d->bus >> 4) & 0xF];
  out[6] = h[d->bus & 0xF];
  out[7] = ':';
  out[8] = h[(d->slot >> 4) & 0xF];
  out[9] = h[d->slot & 0xF];
  out[10] = '.';
  out[11] = h[d->func & 0xF];
  out[12] = 0;
}

/* idx-th PCI device's BDF into out; 1 if it exists. */
static bool pci_bdf(int idx, char *out)
{
  struct pci_dev_info d;
  if (pci_dev_info(idx, &d) <= 0)
    return false;
  pci_bdf_str(&d, out);
  return true;
}

/* Resolve a BDF dir name to its device info; returns the index, or -1. */
static int pci_find(const char *name, struct pci_dev_info *out)
{
  struct pci_dev_info d;
  char b[16];
  for (int i = 0; pci_dev_info(i, &d) > 0; i++) {
    pci_bdf_str(&d, b);
    if (eq(b, name)) {
      if (out)
        *out = d;
      return i;
    }
  }
  return -1;
}

/* ------------------------------------------------------------- classification
 */
enum { S_NONE = 0, S_DIR, S_FILE_RO, S_FILE_RW };

/* Resolve a tokenised path to a directory, an attribute file, or nothing. */
static int classify(char seg[MAXSEG][SEGLEN], int n)
{
  if (n == 0)
    return S_DIR; /* /sys */

  if (n == 1) {
    if (eq(seg[0], "class") || eq(seg[0], "bus") || eq(seg[0], "power") || eq(seg[0], "firmware"))
      return S_DIR;
    return S_NONE;
  }

  if (eq(seg[0], "bus")) { /* /sys/bus/pci/devices/... */
    if (n == 2)
      return eq(seg[1], "pci") ? S_DIR : S_NONE;
    if (eq(seg[1], "pci")) {
      if (n == 3)
        return eq(seg[2], "devices") ? S_DIR : S_NONE;
      if (eq(seg[2], "devices")) {
        if (n == 4)
          return pci_find(seg[3], 0) >= 0 ? S_DIR : S_NONE;
        if (n == 5 && pci_find(seg[3], 0) >= 0 && (eq(seg[4], "vendor") || eq(seg[4], "device") || eq(seg[4], "class")))
          return S_FILE_RO;
      }
    }
    return S_NONE;
  }

  if (eq(seg[0], "class")) {
    if (n == 2)
      return (eq(seg[1], "input") || eq(seg[1], "net") || eq(seg[1], "block") || eq(seg[1], "graphics") ||
              eq(seg[1], "dmi") || (eq(seg[1], "sound") && audio_volume_present()) ||
              (eq(seg[1], "backlight") && backlight_present()) || (eq(seg[1], "bluetooth") && bt_present()) ||
              (eq(seg[1], "video") && uvc_present()))
                 ? S_DIR
                 : S_NONE;
    if (eq(seg[1], "input")) {
      if (n == 3)
        return (eq(seg[2], "kbd0") || eq(seg[2], "mouse0") || (eq(seg[2], "js0") && gamepad_present())) ? S_DIR
                                                                                                        : S_NONE;
      if (n == 4 && eq(seg[2], "kbd0") && eq(seg[3], "layout"))
        return S_FILE_RW;
      if (n == 4 && eq(seg[2], "mouse0") && eq(seg[3], "speed"))
        return S_FILE_RW;
      if (n == 4 && eq(seg[2], "js0") && eq(seg[3], "state") && gamepad_present())
        return S_FILE_RO;
      if (n == 4 && eq(seg[2], "js0") && eq(seg[3], "rumble") && gamepad_present())
        return S_FILE_RW;
      if (n == 4 && eq(seg[2], "js0") && eq(seg[3], "id") && gamepad_present())
        return S_FILE_RO;
      return S_NONE;
    }
    if (eq(seg[1], "net")) {
      if (!nic_present())
        return S_NONE;
      if (n == 3)
        return eq(seg[2], NET_IFACE) ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], NET_IFACE) &&
          (eq(seg[3], "address") || eq(seg[3], "carrier") || eq(seg[3], "ipaddr") || eq(seg[3], "ip6addr") ||
           eq(seg[3], "ip6ll") || eq(seg[3], "dns6")))
        return S_FILE_RO;
      return S_NONE;
    }
    if (eq(seg[1], "block")) {
      if (n == 3)
        return block_exists(seg[2]) ? S_DIR : S_NONE;
      if (n == 4 && block_exists(seg[2]) && (eq(seg[3], "model") || eq(seg[3], "size")))
        return S_FILE_RO;
      return S_NONE;
    }
    if (eq(seg[1], "graphics")) {
      if (n == 3)
        return eq(seg[2], "fb0") ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], "fb0")) {
        if (eq(seg[3], "width") || eq(seg[3], "height") || eq(seg[3], "refresh") || eq(seg[3], "monitor") ||
            eq(seg[3], "internal") || eq(seg[3], "modes") || eq(seg[3], "dpi") || eq(seg[3], "scale_active") ||
            eq(seg[3], "hdmi") || eq(seg[3], "hdmi_audio") || eq(seg[3], "connected"))
          return S_FILE_RO;
        if (eq(seg[3], "mode")) /* rw only where we can drive the mode */
          return fb_can_set_mode() ? S_FILE_RW : S_FILE_RO;
        if (eq(seg[3], "rate") || eq(seg[3], "brightness") ||
            eq(seg[3], "scale")) /* rate: compositor frame-pacing target (Hz) */
          return S_FILE_RW;
      }
      return S_NONE;
    }
    if (eq(seg[1], "backlight")) {
      if (!backlight_present())
        return S_NONE;
      if (n == 3)
        return eq(seg[2], "intel_backlight") ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], "intel_backlight")) {
        if (eq(seg[3], "brightness"))
          return S_FILE_RW;
        if (eq(seg[3], "actual_brightness") || eq(seg[3], "max_brightness") || eq(seg[3], "type"))
          return S_FILE_RO;
      }
      return S_NONE;
    }
    if (eq(seg[1], "dmi")) {
      if (n == 3)
        return eq(seg[2], "id") ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], "id") && (eq(seg[3], "product_name") || eq(seg[3], "sys_vendor")))
        return S_FILE_RO;
      return S_NONE;
    }
    if (eq(seg[1], "video")) {
      if (!uvc_present())
        return S_NONE;
      if (n == 3)
        return eq(seg[2], "cam0") ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], "cam0")) {
        if (eq(seg[3], "control"))
          return S_FILE_RW;
        if (eq(seg[3], "info") || eq(seg[3], "frame"))
          return S_FILE_RO;
      }
      return S_NONE;
    }
    if (eq(seg[1], "bluetooth")) {
      if (!bt_present())
        return S_NONE;
      if (n == 3)
        return eq(seg[2], "hci0") ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], "hci0")) {
        if (eq(seg[3], "control"))
          return S_FILE_RW;
        if (eq(seg[3], "status") || eq(seg[3], "devices") || eq(seg[3], "le_devices") || eq(seg[3], "gatt") ||
            eq(seg[3], "gatt_events"))
          return S_FILE_RO;
      }
      return S_NONE;
    }
    if (eq(seg[1], "sound")) {
      if (!audio_volume_present())
        return S_NONE;
      if (n == 3)
        return eq(seg[2], "card0") ? S_DIR : S_NONE;
      if (n == 4 && eq(seg[2], "card0")) {
        if (eq(seg[3], "output"))
          return S_FILE_RW;
        if (eq(seg[3], "outputs") || eq(seg[3], "muted"))
          return S_FILE_RO;
        if (eq(seg[3], "volume"))
          return S_FILE_RW;
        if (eq(seg[3], "capture") && hda_capture_present())
          return S_FILE_RW;
        if (eq(seg[3], "capture_pcm") && hda_capture_present())
          return S_FILE_RO;
      }
      return S_NONE;
    }
    return S_NONE;
  }

  if (eq(seg[0], "power")) {
    if (!battery_present())
      return S_NONE;
    if (n == 2)
      return eq(seg[1], "battery") ? S_DIR : S_NONE;
    if (n == 3 && eq(seg[1], "battery") &&
        (eq(seg[2], "capacity") || eq(seg[2], "status") || eq(seg[2], "minutes") || eq(seg[2], "diag")))
      return S_FILE_RO;
    return S_NONE;
  }

  if (eq(seg[0], "firmware")) {
    if (n == 2 && eq(seg[1], "type"))
      return S_FILE_RO;
    return S_NONE;
  }

  return S_NONE;
}

/* ------------------------------------------------------------- attribute read
 */
/* classify() has already validated the path depth, so emit only needs the
 * segments - it reads whichever attribute the leaf names. */
static void emit(char seg[MAXSEG][SEGLEN], struct pb *b)
{
  if (eq(seg[0], "firmware")) {
    ps(b, firmware_name());
    pc(b, '\n');
    return;
  }

  if (eq(seg[0], "class")) {
    if (eq(seg[1], "input")) {
      if (eq(seg[2], "kbd0")) {
        ps(b, keyboard_layout_name());
        pc(b, '\n');
        return;
      }
      if (eq(seg[2], "mouse0")) {
        pu(b, (uint32_t)mouse_speed());
        pc(b, '\n');
        return;
      }
      if (eq(seg[2], "js0") && eq(seg[3], "rumble")) {
        ps(b, hid_gamepad_can_rumble() ? "1\n" : "0\n");
        return;
      }
      if (eq(seg[2], "js0") && eq(seg[3], "id")) {
        char id[64];
        if (hid_gamepad_id(id, sizeof id) >= 0)
          ps(b, id);
        return;
      }
      if (eq(seg[2], "js0") && eq(seg[3], "state")) {
        char st[96];
        if (hid_gamepad_state(st, sizeof st) >= 0)
          ps(b, st);
        return;
      }
    }
    if (eq(seg[1], "net")) {
      if (eq(seg[3], "address")) {
        const uint8_t *m = nic_mac();
        for (int i = 0; i < 6; i++) {
          phex2(b, m[i]);
          if (i < 5)
            pc(b, ':');
        }
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "carrier")) {
        ps(b, nic_present() ? "1" : "0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "ipaddr")) {
        const uint8_t *ip = net_my_ip();
        if (ip) {
          pu(b, ip[0]);
          pc(b, '.');
          pu(b, ip[1]);
          pc(b, '.');
          pu(b, ip[2]);
          pc(b, '.');
          pu(b, ip[3]);
        } else
          ps(b, "0.0.0.0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "ip6addr") || eq(seg[3], "ip6ll") || eq(seg[3], "dns6")) {
        const uint8_t *ip = eq(seg[3], "ip6ll") ? net_ll_ip6() : eq(seg[3], "dns6") ? net_dns6() : net_my_ip6();
        char str[40];
        if (ip) { /* a global address is empty until a router advertisement arrives */
          net_addr_str(ip, str);
          ps(b, str);
        }
        pc(b, '\n');
        return;
      }
    }
    if (eq(seg[1], "block")) {
      block_attr(seg[2], b, eq(seg[3], "size"));
      return;
    }
    if (eq(seg[1], "graphics")) {
      if (eq(seg[3], "width")) {
        pu(b, fb_active() ? fb_width() : 0);
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "height")) {
        pu(b, fb_active() ? fb_height() : 0);
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "refresh")) {
        int hz = edid_refresh_hz();
        if (hz <= 0)
          hz = fb_active() ? 60 : 70;
        pu(b, (uint32_t)hz);
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "monitor")) {
        char mon[48];
        edid_model(mon, (int)sizeof mon);
        ps(b, mon);
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "internal")) {
        ps(b, edid_internal() ? "1" : "0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "hdmi")) {
        ps(b, edid_hdmi() ? "1" : "0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "hdmi_audio")) {
        ps(b, edid_hdmi_audio() ? "1" : "0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "connected")) {
        edid_rescan();
        ps(b, edid_connected() ? "1" : "0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "dpi")) { /* EDID pixels per inch, 0 if unknown */
        pu(b, (uint32_t)edid_dpi());
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "scale")) { /* the UI scale setting: "auto" or 1..4 */
        if (fb_ui_scale())
          pu(b, (uint32_t)fb_ui_scale());
        else
          ps(b, "auto");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "scale_active")) { /* the factor the compositor applies */
        pu(b, (uint32_t)fb_ui_scale_active());
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "rate")) { /* compositor frame-pacing target (Hz)  */
        pu(b, (uint32_t)fb_target_hz());
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "brightness")) {
        pu(b, (uint32_t)fb_brightness_percent());
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "mode")) { /* the current mode, "WxH" */
        pu(b, fb_active() ? fb_width() : 0);
        pc(b, 'x');
        pu(b, fb_active() ? fb_height() : 0);
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "modes")) { /* one "WxH" per line; index = row     */
        /* Only enumerate the switchable list where we can actually drive
         * the mode (DISPI). On an adopted/GOP framebuffer the resolution is
         * fixed, so just report the current one. */
        if (fb_can_set_mode()) {
          uint32_t w, h;
          for (int i = 0; fb_mode_get(i, &w, &h); i++) {
            pu(b, w);
            pc(b, 'x');
            pu(b, h);
            pc(b, '\n');
          }
        } else if (fb_active()) {
          pu(b, fb_width());
          pc(b, 'x');
          pu(b, fb_height());
          pc(b, '\n');
        }
        return;
      }
    }
    if (eq(seg[1], "backlight")) { /* raw PWM units, as Linux's intel_backlight */
      if (eq(seg[3], "type")) {
        ps(b, "raw\n");
        return;
      }
      if (eq(seg[3], "brightness"))
        pu(b, backlight_raw());
      else if (eq(seg[3], "actual_brightness"))
        pu(b, backlight_actual());
      else
        pu(b, backlight_max());
      pc(b, '\n');
      return;
    }
    if (eq(seg[1], "dmi")) {
      if (eq(seg[3], "product_name")) {
        const char *s = smbios_product();
        ps(b, s ? s : "");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "sys_vendor")) {
        const char *s = smbios_vendor();
        ps(b, s ? s : "");
        pc(b, '\n');
        return;
      }
    }
    if (eq(seg[1], "video")) {
      int room = (int)(b->end - b->p);
      if (eq(seg[3], "info"))
        b->p += uvc_info_text(b->p, room);
      else if (eq(seg[3], "control"))
        b->p += uvc_control_text(b->p, room);
      return;
    }
    if (eq(seg[1], "bluetooth")) {
      int room = (int)(b->end - b->p);
      if (eq(seg[3], "status"))
        b->p += bt_status_text(b->p, room);
      else if (eq(seg[3], "devices"))
        b->p += bt_devices_text(b->p, room);
      else if (eq(seg[3], "le_devices"))
        b->p += bt_le_devices_text(b->p, room);
      else if (eq(seg[3], "gatt"))
        b->p += bt_gatt_text(b->p, room);
      else if (eq(seg[3], "gatt_events"))
        b->p += bt_gatt_events_text(b->p, room);
      return;
    }
    if (eq(seg[1], "sound")) {
      char nm[48];
      if (eq(seg[3], "volume")) { /* 0..100 */
        pu(b, (uint32_t)((audio_volume_level() * 100 + AUDIO_VOL_MAX / 2) / AUDIO_VOL_MAX));
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "muted")) {
        ps(b, audio_volume_muted() ? "1" : "0");
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "output")) { /* the active output's name */
        int a = hda_output_active();
        if (a >= 0 && hda_output_name(a, nm, (int)sizeof nm))
          ps(b, nm);
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "capture")) {
        if (hda_capture_active()) {
          ps(b, "capturing 48000 1\npos ");
          pu(b, hda_capture_pos());
        } else {
          ps(b, "idle\npos 0");
        }
        pc(b, '\n');
        return;
      }
      if (eq(seg[3], "outputs")) { /* one name per line, index = row */
        int cnt = hda_output_count();
        for (int i = 0; i < cnt; i++)
          if (hda_output_name(i, nm, (int)sizeof nm)) {
            ps(b, nm);
            pc(b, '\n');
          }
        return;
      }
    }
  }

  if (eq(seg[0], "bus") && eq(seg[1], "pci") && eq(seg[2], "devices")) {
    struct pci_dev_info d;
    if (pci_find(seg[3], &d) >= 0) {
      if (eq(seg[4], "vendor")) {
        ps(b, "0x");
        phex4(b, d.vendor);
        pc(b, '\n');
        return;
      }
      if (eq(seg[4], "device")) {
        ps(b, "0x");
        phex4(b, d.device);
        pc(b, '\n');
        return;
      }
      if (eq(seg[4], "class")) { /* class:subclass:progif, Linux 6-hex   */
        ps(b, "0x");
        phex2(b, d.class_code);
        phex2(b, d.subclass);
        phex2(b, 0);
        pc(b, '\n');
        return;
      }
    }
    return;
  }

  if (eq(seg[0], "power") && eq(seg[1], "battery")) {
    int charging = 0, discharging = 0, minutes = 0;
    if (eq(seg[2], "capacity")) {
      int pct = battery_percent(&charging, &discharging, &minutes);
      pu(b, pct < 0 ? 0 : (uint32_t)pct);
      pc(b, '\n');
      return;
    }
    if (eq(seg[2], "status")) {
      (void)battery_percent(&charging, &discharging, &minutes);
      ps(b, charging ? "Charging" : discharging ? "Discharging" : "Full");
      pc(b, '\n');
      return;
    }
    if (eq(seg[2], "minutes")) { /* runtime remaining, 0 if unknown      */
      (void)battery_percent(&charging, &discharging, &minutes);
      pu(b, minutes < 0 ? 0 : (uint32_t)minutes);
      pc(b, '\n');
      return;
    }
    if (eq(seg[2], "diag")) { /* AML detection note (real-HW debug)   */
      ps(b, battery_diag());
      pc(b, '\n');
      return;
    }
  }
}

/* ------------------------------------------------------------- attribute write
 */
static int apply(char seg[MAXSEG][SEGLEN], int n, const char *arg)
{
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "input")) {
    if (eq(seg[2], "kbd0") && eq(seg[3], "layout"))
      return keyboard_set_layout(arg) == 0 ? 0 : -1;
    if (eq(seg[2], "mouse0") && eq(seg[3], "speed")) {
      int v = 0;
      for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
      mouse_set_speed(v);
      return 0;
    }
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "input") && eq(seg[2], "js0") && eq(seg[3], "rumble")) {
    uint32_t v[3] = {0, 0, 0};
    const char *p = arg;
    for (int i = 0; i < 3; i++) {
      while (*p == ' ')
        p++;
      if (*p < '0' || *p > '9')
        return -1;
      for (; *p >= '0' && *p <= '9'; p++)
        v[i] = v[i] > 0xFFFFFF ? v[i] : v[i] * 10 + (uint32_t)(*p - '0');
    }
    return hid_gamepad_rumble(v[0] > 65535 ? 65535 : (uint16_t)v[0], v[1] > 65535 ? 65535 : (uint16_t)v[1], v[2]) ? 0
                                                                                                                  : -1;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "bluetooth") && eq(seg[2], "hci0") && eq(seg[3], "control"))
    return bt_command(arg) == 0 ? 0 : -1;
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "video") && eq(seg[2], "cam0") && eq(seg[3], "control"))
    return uvc_command(arg) == 0 ? 0 : -1;
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "sound") && eq(seg[2], "card0") && eq(seg[3], "output")) {
    /* Accept an index ("1") or a name ("Headphones"). */
    int idx = -1;
    if (arg[0] >= '0' && arg[0] <= '9') {
      idx = 0;
      for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
        idx = idx * 10 + (*p - '0');
    } else {
      char nm[48];
      for (int i = 0, cnt = hda_output_count(); i < cnt; i++)
        if (hda_output_name(i, nm, (int)sizeof nm) && eq(nm, arg)) {
          idx = i;
          break;
        }
    }
    return hda_output_select(idx) == 0 ? 0 : -1;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "sound") && eq(seg[2], "card0") && eq(seg[3], "capture")) {
    if (eq(arg, "start"))
      return hda_capture_start();
    return eq(arg, "stop") ? hda_capture_stop() : -1;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "sound") && eq(seg[2], "card0") && eq(seg[3], "volume")) {
    int v = parse_percent(arg);
    if (v < 0)
      return -1;
    audio_volume_set((v * AUDIO_VOL_MAX + 50) / 100);
    return 0;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "graphics") && eq(seg[2], "fb0") && eq(seg[3], "brightness")) {
    int v = parse_percent(arg);
    if (v < 0)
      return -1;
    fb_set_brightness_percent(v);
    return 0;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "backlight") && eq(seg[2], "intel_backlight") &&
      eq(seg[3], "brightness")) {
    uint32_t v;
    if (!backlight_present() || parse_u32(arg, &v) < 0)
      return -1;
    backlight_set_raw(v);
    return 0;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "graphics") && eq(seg[2], "fb0") && eq(seg[3], "mode")) {
    /* "WxH": reprogram the adapter directly (only DISPI is switchable, gated by
     * fb_can_set_mode). The compositor re-queries fb_info each frame and
     * rebinds its surface on a change, so a live resolution switch takes effect
     * at once. */
    int w = 0, h = 0;
    const char *p = arg;
    while (*p >= '0' && *p <= '9')
      w = w * 10 + (*p++ - '0');
    if (*p != 'x' && *p != 'X')
      return -1;
    p++;
    while (*p >= '0' && *p <= '9')
      h = h * 10 + (*p++ - '0');
    if (w <= 0 || h <= 0 || !fb_can_set_mode())
      return -1;
    return fb_set_mode((uint32_t)w, (uint32_t)h);
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "graphics") && eq(seg[2], "fb0") && eq(seg[3], "scale")) {
    if (arg[0] == 'a') {
      fb_set_ui_scale(0);
      return 0;
    }
    if (arg[0] < '1' || arg[0] > '4')
      return -1;
    fb_set_ui_scale(arg[0] - '0');
    return 0;
  }
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "graphics") && eq(seg[2], "fb0") && eq(seg[3], "rate")) {
    int hz = 0;
    const char *p = arg;
    while (*p >= '0' && *p <= '9')
      hz = hz * 10 + (*p++ - '0');
    if (hz <= 0)
      return -1;
    fb_set_target_hz(hz); /* clamps; the compositor polls it and re-paces */
    return 0;
  }
  return -1;
}

/* ------------------------------------------------------------- enumeration */
/* List a static name array, returning the idx-th into out (1 if in range). */
static bool from_list(int idx, char *out, int max, const char *const *names, int n)
{
  if (idx < 0 || idx >= n)
    return false;
  cpy(out, names[idx], max);
  return true;
}

int sysfs_enum(const char *dirsub, int idx, char *name, int max, int *is_dir)
{
  char seg[MAXSEG][SEGLEN];
  int n = split(dirsub, seg);
  int hit = 0;

  if (n == 0) { /* /sys */
    static const char *const top[] = {"class", "bus", "power", "firmware"};
    hit = from_list(idx, name, max, top, 4);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }

  if (n == 1 && eq(seg[0], "class")) {
    /* input + graphics always; net/block only when present (so an empty dir
     * doesn't show). fb0 exists even in text mode, like /proc/display did. */
    const char *kids[9];
    int k = 0;
    kids[k++] = "input";
    if (nic_present())
      kids[k++] = "net";
    char tmp[SEGLEN];
    if (block_name(0, tmp))
      kids[k++] = "block";
    kids[k++] = "graphics";
    if (backlight_present())
      kids[k++] = "backlight";
    if (audio_volume_present())
      kids[k++] = "sound";
    if (bt_present())
      kids[k++] = "bluetooth";
    if (uvc_present())
      kids[k++] = "video";
    kids[k++] = "dmi";
    hit = from_list(idx, name, max, kids, k);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 1 && eq(seg[0], "bus")) {
    static const char *const k[] = {"pci"};
    hit = from_list(idx, name, max, k, 1);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 1 && eq(seg[0], "power")) {
    if (!battery_present())
      return 0;
    static const char *const k[] = {"battery"};
    hit = from_list(idx, name, max, k, 1);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 1 && eq(seg[0], "firmware")) {
    static const char *const k[] = {"type"};
    hit = from_list(idx, name, max, k, 1);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }

  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "input")) {
    static const char *const k[] = {"kbd0", "mouse0", "js0"};
    hit = from_list(idx, name, max, k, gamepad_present() ? 3 : 2);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "net")) {
    if (!nic_present() || idx != 0)
      return 0;
    cpy(name, NET_IFACE, max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "block")) {
    hit = block_name(idx, name);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "graphics")) {
    if (idx != 0)
      return 0;
    cpy(name, "fb0", max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "backlight")) {
    if (!backlight_present() || idx != 0)
      return 0;
    cpy(name, "intel_backlight", max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "dmi")) {
    if (idx != 0)
      return 0;
    cpy(name, "id", max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "bluetooth")) {
    if (!bt_present() || idx != 0)
      return 0;
    cpy(name, "hci0", max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "video")) {
    if (!uvc_present() || idx != 0)
      return 0;
    cpy(name, "cam0", max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "video") && eq(seg[2], "cam0") && uvc_present()) {
    static const char *const k[] = {"info", "control", "frame"};
    hit = from_list(idx, name, max, k, 3);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "bluetooth") && eq(seg[2], "hci0")) {
    static const char *const k[] = {"status", "devices", "le_devices", "gatt", "gatt_events", "control"};
    hit = from_list(idx, name, max, k, 6);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 2 && eq(seg[0], "class") && eq(seg[1], "sound")) {
    if (!audio_volume_present() || idx != 0)
      return 0;
    cpy(name, "card0", max);
    if (is_dir)
      *is_dir = 1;
    return 1;
  }
  if (n == 2 && eq(seg[0], "bus") && eq(seg[1], "pci")) {
    static const char *const k[] = {"devices"};
    hit = from_list(idx, name, max, k, 1);
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 2 && eq(seg[0], "power") && eq(seg[1], "battery")) {
    static const char *const k[] = {"capacity", "status", "minutes", "diag"};
    hit = from_list(idx, name, max, k, 4);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }

  if (n == 3 && eq(seg[0], "bus") && eq(seg[1], "pci") && eq(seg[2], "devices")) {
    hit = pci_bdf(idx, name); /* one dir per PCI device (BDF)     */
    if (hit && is_dir)
      *is_dir = 1;
    return hit;
  }
  if (n == 4 && eq(seg[0], "bus") && eq(seg[1], "pci") && eq(seg[2], "devices")) {
    if (pci_find(seg[3], 0) < 0)
      return 0;
    static const char *const k[] = {"vendor", "device", "class"};
    hit = from_list(idx, name, max, k, 3);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }

  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "input") && eq(seg[2], "kbd0")) {
    static const char *const k[] = {"layout"};
    hit = from_list(idx, name, max, k, 1);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "input") && eq(seg[2], "js0") && gamepad_present()) {
    static const char *const k[] = {"state", "rumble", "id"};
    hit = from_list(idx, name, max, k, 3);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "input") && eq(seg[2], "mouse0")) {
    static const char *const k[] = {"speed"};
    hit = from_list(idx, name, max, k, 1);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "net")) {
    static const char *const k[] = {"address", "carrier", "ipaddr", "ip6addr", "ip6ll", "dns6"};
    hit = from_list(idx, name, max, k, 6);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "block")) {
    static const char *const k[] = {"model", "size"};
    hit = from_list(idx, name, max, k, 2);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "graphics") && eq(seg[2], "fb0")) {
    static const char *const k[] = {"width", "height",       "refresh", "monitor",    "internal",
                                    "mode",  "modes",        "rate",    "brightness", "dpi",
                                    "scale", "scale_active", "hdmi",    "hdmi_audio", "connected"};
    hit = from_list(idx, name, max, k, 15);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "backlight") && eq(seg[2], "intel_backlight")) {
    if (!backlight_present())
      return 0;
    static const char *const k[] = {"brightness", "actual_brightness", "max_brightness", "type"};
    hit = from_list(idx, name, max, k, 4);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "dmi") && eq(seg[2], "id")) {
    static const char *const k[] = {"product_name", "sys_vendor"};
    hit = from_list(idx, name, max, k, 2);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }
  if (n == 3 && eq(seg[0], "class") && eq(seg[1], "sound") && eq(seg[2], "card0")) {
    static const char *const k[] = {"output", "outputs", "volume", "muted", "capture", "capture_pcm"};
    hit = from_list(idx, name, max, k, hda_capture_present() ? 6 : 4);
    if (hit && is_dir)
      *is_dir = 0;
    return hit;
  }

  return 0;
}

/* ------------------------------------------------------------- public API */
const char *sysfs_subpath(const char *path)
{
  if (!path)
    return 0;
  while (*path == '/')
    path++;
  const char *q = "sys";
  while (*q && *path == *q) {
    path++;
    q++;
  }
  if (*q)
    return 0; /* didn't match "sys"               */
  if (*path && *path != '/')
    return 0;  /* "sysfoo" is not /sys             */
  return path; /* "" or "/class/..."              */
}

int sysfs_kind(const char *sub)
{
  char seg[MAXSEG][SEGLEN];
  int n = split(sub, seg);
  int c = classify(seg, n);
  if (c == S_DIR)
    return 1;
  if (c == S_FILE_RO || c == S_FILE_RW)
    return 0;
  return -1;
}

int sysfs_read(const char *sub, char *buf, int max)
{
  __asm__ volatile("sti"); /* battery/ACPI reads wait on the timer */
  char seg[MAXSEG][SEGLEN];
  int n = split(sub, seg);
  int c = classify(seg, n);
  if (c != S_FILE_RO && c != S_FILE_RW)
    return -1;
  struct pb b = {buf, buf + max};
  emit(seg, &b);
  return (int)(b.p - buf);
}

int sysfs_writable(const char *sub)
{
  char seg[MAXSEG][SEGLEN];
  int n = split(sub, seg);
  return classify(seg, n) == S_FILE_RW;
}

int sysfs_write(const char *sub, const char *buf, int len)
{
  __asm__ volatile("sti");
  char seg[MAXSEG][SEGLEN];
  int n = split(sub, seg);
  if (classify(seg, n) != S_FILE_RW)
    return -1;

  char arg[800];
  int m = len;
  if (m > (int)sizeof arg - 1)
    m = (int)sizeof arg - 1;
  for (int i = 0; i < m; i++)
    arg[i] = buf[i];
  arg[m] = 0;
  while (m > 0 && (arg[m - 1] == '\n' || arg[m - 1] == '\r'))
    arg[--m] = 0;

  return apply(seg, n, arg) == 0 ? len : -1;
}

int sysfs_read_binary(const char *sub, uint32_t off, void *buf, uint32_t len)
{
  char seg[MAXSEG][SEGLEN];
  int n = split(sub, seg);
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "video") && eq(seg[2], "cam0") && eq(seg[3], "frame") &&
      uvc_present())
    return uvc_frame_read(off, buf, len);
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "sound") && eq(seg[2], "card0") && eq(seg[3], "capture_pcm") &&
      hda_capture_present())
    return hda_capture_read(off, buf, len);
  return SYSFS_NOT_BINARY;
}

int sysfs_size(const char *sub)
{
  static char scratch[4096];
  char seg[MAXSEG][SEGLEN];
  int n = split(sub, seg);
  if (n == 4 && eq(seg[0], "class") && eq(seg[1], "video") && eq(seg[2], "cam0") && eq(seg[3], "frame") &&
      uvc_present())
    return (int)uvc_frame_size();
  n = sysfs_read(sub, scratch, (int)sizeof scratch);
  return n < 0 ? 0 : n;
}

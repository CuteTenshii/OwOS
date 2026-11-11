/* usb3.c: SuperSpeed and SuperSpeedPlus link-speed parsing (see usb3.h). */

#include "usb3.h"
#include <stddef.h>

#if __STDC_HOSTED__
#include <string.h>
#else
#include "kstring.h"
#endif

#define XHCI_CAP_PROTOCOL 2
#define PROTO_NAME_USB 0x20425355u /* "USB " */

#define DT_BOS 0x0F
#define DT_DEVICE_CAPABILITY 0x10
#define CAP_SUPERSPEED 0x03
#define CAP_SUPERSPEED_PLUS 0x0A

#define LP_SUPERSPEED_PLUS 1

/* A rate given as mantissa and exponent (0 b/s, 1 Kb/s, 2 Mb/s, 3 Gb/s), in kb/s, saturating. */
static uint32_t rate_kbps(uint32_t mantissa, uint32_t exponent)
{
  static const uint32_t scale[] = {0, 1, 1000, 1000000};
  if (exponent == 0)
    return mantissa / 1000;
  uint64_t v = (uint64_t)mantissa * scale[exponent & 3];
  return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

int usb3_proto_parse(const uint32_t *dw, int ndw, struct usb3_proto *out)
{
  memset(out, 0, sizeof *out);
  if (ndw < 4 || (dw[0] & 0xFF) != XHCI_CAP_PROTOCOL || dw[1] != PROTO_NAME_USB)
    return -1;
  out->minor = (uint8_t)(dw[0] >> 16);
  out->major = (uint8_t)(dw[0] >> 24);
  out->port_first = (uint8_t)dw[2];
  out->port_count = (uint8_t)(dw[2] >> 8);
  uint32_t psic = dw[2] >> 28;
  if ((int)(4 + psic) > ndw)
    return -1;
  for (uint32_t i = 0; i < psic && out->npsi < USB3_MAX_PSI; i++) {
    uint32_t v = dw[4 + i];
    struct usb3_psi *s = &out->psi[out->npsi];
    s->psiv = v & 0xF;
    if (!s->psiv)
      continue;
    uint32_t plt = (v >> 6) & 3;
    s->asym = plt >= 2;
    s->tx = plt == 3;
    s->ssp = ((v >> 14) & 3) == LP_SUPERSPEED_PLUS;
    s->kbps = rate_kbps(v >> 16, (v >> 4) & 3);
    out->npsi++;
  }
  return 0;
}

/* The xHCI default speed ids, which apply when a protocol lists no PSI dwords: Gen 2 needs USB 3.1, dual-lane 3.2. A
 * major of 0 means the port's revision is unknown and allows every id. */
static int default_speed(int major, int minor, uint8_t psiv, uint32_t *kbps)
{
  static const struct {
    int speed;
    uint32_t kbps;
    int min_minor;
  } ids[] = {
      {0, 0, 0},
      {USB_SPEED_FULL, 12000, 0},
      {USB_SPEED_LOW, 1500, 0},
      {USB_SPEED_HIGH, 480000, 0},
      {USB_SPEED_SUPER, 5000000, 0},
      {USB_SPEED_SUPER_PLUS, 10000000, 0x10}, /* Gen 2x1 */
      {USB_SPEED_SUPER_PLUS, 10000000, 0x20}, /* Gen 1x2 */
      {USB_SPEED_SUPER_PLUS, 20000000, 0x20}, /* Gen 2x2 */
  };
  if (psiv == 0 || psiv >= sizeof ids / sizeof ids[0])
    return 0;
  if (major == 2 && psiv > 3)
    return 0;
  if (major >= 3 && psiv < 4)
    return 0;
  if (major == 3 && minor < ids[psiv].min_minor)
    return 0;
  *kbps = ids[psiv].kbps;
  return ids[psiv].speed;
}

int usb3_port_speed(const struct usb3_proto *p, uint8_t psiv, uint32_t *kbps)
{
  uint32_t rate = 0;
  int speed = 0;
  if (!p || !p->npsi) {
    speed = default_speed(p ? p->major : 0, p ? p->minor : 0, psiv, &rate);
  } else {
    const struct usb3_psi *hit = NULL;
    for (int i = 0; i < p->npsi; i++)
      if (p->psi[i].psiv == psiv && (!hit || hit->tx))
        hit = &p->psi[i];
    if (!hit)
      return 0;
    rate = hit->kbps;
    if (p->major >= 3)
      speed = hit->ssp ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
    else
      speed = rate <= 1500 ? USB_SPEED_LOW : rate <= 12000 ? USB_SPEED_FULL : USB_SPEED_HIGH;
  }
  if (speed && kbps)
    *kbps = rate;
  return speed;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void parse_ssp_cap(const uint8_t *c, int n, struct usb3_bos *out)
{
  if (n < 12)
    return;
  uint32_t attrs = le32(c + 4);
  uint32_t func = c[8] | c[9] << 8;
  int count = (int)(attrs & 0x1F) + 1;
  out->ssp = true;
  out->min_rx_lanes = (uint8_t)((func >> 8) & 0xF);
  out->min_tx_lanes = (uint8_t)((func >> 12) & 0xF);
  const uint8_t *attr = c + 12;
  for (int i = 0; i < count && attr + 4 <= c + n && out->nsublinks < USB3_MAX_SUBLINKS; i++, attr += 4) {
    uint32_t v = le32(attr);
    struct usb3_sublink *s = &out->sublinks[out->nsublinks++];
    s->ssid = v & 0xF;
    s->asym = (v >> 6) & 1;
    s->tx = (v >> 7) & 1;
    s->ssp = ((v >> 14) & 3) == LP_SUPERSPEED_PLUS;
    s->kbps = rate_kbps(v >> 16, (v >> 4) & 3);
  }
}

int usb3_bos_parse(const uint8_t *bos, int len, struct usb3_bos *out)
{
  memset(out, 0, sizeof *out);
  if (len < 5 || bos[0] < 5 || bos[1] != DT_BOS)
    return -1;
  int total = bos[2] | bos[3] << 8;
  if (total < len)
    len = total;
  for (int i = bos[0]; i + 3 <= len;) {
    int n = bos[i];
    if (n < 3 || i + n > len)
      break;
    if (bos[i + 1] == DT_DEVICE_CAPABILITY) {
      if (bos[i + 2] == CAP_SUPERSPEED)
        out->ss = true;
      else if (bos[i + 2] == CAP_SUPERSPEED_PLUS && !out->ssp)
        parse_ssp_cap(bos + i, n, out);
    }
    i += n;
  }
  return 0;
}

uint32_t usb3_bos_max_kbps(const struct usb3_bos *b)
{
  uint32_t best = 0;
  if (!b->ssp)
    return 0;
  for (int i = 0; i < b->nsublinks; i++) {
    const struct usb3_sublink *s = &b->sublinks[i];
    if (s->ssp && !(s->asym && s->tx) && s->kbps > best)
      best = s->kbps;
  }
  return best;
}

int usb3_ext_port_speed(const struct usb3_bos *hub, uint32_t ext_status, uint32_t *kbps)
{
  uint8_t ssid = ext_status & 0xF;
  uint32_t lanes = ((ext_status >> 8) & 0xF) + 1;
  for (int i = 0; hub && hub->ssp && i < hub->nsublinks; i++) {
    const struct usb3_sublink *s = &hub->sublinks[i];
    if (s->ssid != ssid || (s->asym && s->tx))
      continue;
    uint64_t rate = (uint64_t)s->kbps * lanes;
    *kbps = rate > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rate;
    return s->ssp ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
  }
  *kbps = 5000000;
  return USB_SPEED_SUPER;
}

uint32_t usb3_default_kbps(int speed)
{
  uint32_t kbps = 0;
  default_speed(speed >= USB_SPEED_SUPER ? 3 : 2, 0x10, (uint8_t)speed, &kbps);
  return kbps;
}

static int put_uint(char *buf, int size, int at, uint32_t v)
{
  char tmp[10];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n && at < size - 1)
    buf[at++] = tmp[--n];
  return at;
}

char *usb3_rate_str(uint32_t kbps, char *buf, int size)
{
  static const char *const unit[] = {" kbps", " Mbps", " Gbps"};
  int u = kbps >= 1000000 ? 2 : kbps >= 1000 ? 1 : 0;
  uint32_t div = u == 2 ? 1000000 : u == 1 ? 1000 : 1;
  uint32_t tenths = (uint32_t)(((uint64_t)kbps * 10 + div / 2) / div);
  if (size <= 0)
    return buf;
  int at = put_uint(buf, size, 0, tenths / 10);
  if (tenths % 10 && at < size - 2) {
    buf[at++] = '.';
    buf[at++] = (char)('0' + tenths % 10);
  }
  for (const char *s = unit[u]; *s && at < size - 1; s++)
    buf[at++] = *s;
  buf[at] = 0;
  return buf;
}

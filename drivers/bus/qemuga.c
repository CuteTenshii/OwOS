/* qemuga.c: QEMU guest agent (qemu-ga protocol) over virtio-serial port 2.
 * The host sends JSON commands, one object each; replies are single-line JSON. Only the commands in cmds[] exist.
 * Arguments are read with a flat key scan, not a JSON parser, which is enough for these commands. */

#include "kstring.h"
#include "power.h"
#include "rtc.h"
#include "timeval64.h"
#include "virtioserial.h"
#include "wallclock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

#define QGA_PORT 2
#define CMD_MAX 1024
#define REPLY_MAX 1536
#define SYNC_DELIM 0xFF

static char cmd[CMD_MAX];
static uint32_t cmd_n;
static int depth;
static bool in_str, esc, overflow;

static void reply(const char *fmt, ...)
{
  static char buf[REPLY_MAX];
  va_list ap;
  va_start(ap, fmt);
  int n = kvsnprintf(buf, sizeof buf - 1, fmt, ap);
  va_end(ap);
  if (n < 0 || n >= (int)sizeof buf - 1)
    return;
  buf[n++] = '\n';
  vserial_write(QGA_PORT, buf, (uint32_t)n);
}

static void reply_error(const char *cls, const char *desc)
{
  reply("{\"error\":{\"class\":\"%s\",\"desc\":\"%s\"}}", cls, desc);
}

static const char *skip_ws(const char *p)
{
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

/* Value text after "key": in s, or NULL. */
static const char *find_val(const char *s, const char *key)
{
  char pat[24];
  ksnprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(s, pat);
  if (!p)
    return NULL;
  p = skip_ws(p + strlen(pat));
  return *p == ':' ? skip_ws(p + 1) : NULL;
}

static bool str_val(const char *p, char *out, uint32_t max)
{
  if (!p || *p != '"')
    return false;
  uint32_t n = 0;
  for (p++; *p && *p != '"'; p++)
    if (n < max - 1)
      out[n++] = *p;
  out[n] = '\0';
  return true;
}

static bool num_val(const char *p, uint64_t *out)
{
  if (!p || *p < '0' || *p > '9')
    return false;
  uint64_t v = 0;
  for (; *p >= '0' && *p <= '9'; p++)
    v = v * 10 + (uint64_t)(*p - '0');
  *out = v;
  return true;
}

static void u64_str(char *out, uint64_t v)
{
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    *out++ = tmp[--n];
  *out = '\0';
}

static const char *const supported[] = {"guest-ping",     "guest-sync",     "guest-sync-delimited", "guest-info",
                                        "guest-shutdown", "guest-get-time", "guest-set-time",       "guest-get-osinfo"};

static void cmd_info(void)
{
  static char buf[REPLY_MAX];
  int n = ksnprintf(buf, sizeof buf, "{\"return\":{\"version\":\"0.1.0\",\"supported_commands\":[");
  for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++)
    n += ksnprintf(buf + n, sizeof buf - (uint32_t)n, "%s{\"enabled\":true,\"name\":\"%s\",\"success-response\":%s}",
                   i ? "," : "", supported[i], streq(supported[i], "guest-shutdown") ? "false" : "true");
  ksnprintf(buf + n, sizeof buf - (uint32_t)n, "]}}");
  reply("%s", buf);
}

static void cmd_sync(const char *args, bool delimited)
{
  uint64_t id;
  if (!num_val(find_val(args, "id"), &id)) {
    reply_error("GenericError", "Parameter 'id' is missing");
    return;
  }
  if (delimited) {
    uint8_t d = SYNC_DELIM;
    vserial_write(QGA_PORT, &d, 1);
  }
  char s[24];
  u64_str(s, id);
  reply("{\"return\":%s}", s);
}

static void cmd_shutdown(const char *args)
{
  char mode[16] = "powerdown";
  if (args)
    str_val(find_val(args, "mode"), mode, sizeof mode);
  if (streq(mode, "reboot"))
    power_reset();
  else if (streq(mode, "powerdown") || streq(mode, "halt"))
    power_off();
  else {
    reply_error("GenericError", "mode is invalid (valid values are: halt|powerdown|reboot)");
    return;
  }
  reply_error("GenericError", "shutdown failed");
}

static void cmd_get_time(void)
{
  struct timeval64 tv;
  wallclock_get(&tv);
  char s[24];
  u64_str(s, (uint64_t)tv.tv_sec * 1000000000u + (uint64_t)tv.tv_usec * 1000u);
  reply("{\"return\":%s}", s);
}

static void cmd_set_time(const char *args)
{
  struct timeval64 tv;
  uint64_t ns;
  if (args && num_val(find_val(args, "time"), &ns)) {
    tv.tv_sec = (long long)(ns / 1000000000u);
    tv.tv_usec = (int)(ns % 1000000000u / 1000u);
  } else {
    tv.tv_sec = rtc_epoch();
    tv.tv_usec = 0;
  }
  if (wallclock_set(&tv))
    reply("{\"return\":{}}");
  else
    reply_error("GenericError", "failed to set the clock");
}

static void dispatch(const char *json)
{
  char name[32];
  if (!str_val(find_val(json, "execute"), name, sizeof name)) {
    reply_error("GenericError", "Parameter 'execute' is missing");
    return;
  }
  const char *args = find_val(json, "arguments");
  if (streq(name, "guest-ping"))
    reply("{\"return\":{}}");
  else if (streq(name, "guest-sync") || streq(name, "guest-sync-delimited"))
    cmd_sync(args ? args : "", streq(name, "guest-sync-delimited"));
  else if (streq(name, "guest-info"))
    cmd_info();
  else if (streq(name, "guest-shutdown"))
    cmd_shutdown(args);
  else if (streq(name, "guest-get-time"))
    cmd_get_time();
  else if (streq(name, "guest-set-time"))
    cmd_set_time(args);
  else if (streq(name, "guest-get-osinfo"))
    reply("{\"return\":{\"id\":\"owos\",\"name\":\"OwOS\",\"pretty-name\":\"OwOS\",\"version\":\"%s\","
          "\"kernel-release\":\"%s\"}}",
          OWOS_VERSION, OWOS_VERSION);
  else {
    char msg[64];
    ksnprintf(msg, sizeof msg, "The command %s has not been found", name);
    reply_error("CommandNotFound", msg);
  }
}

/* Collect bytes until a top-level JSON object closes, then run it. Oversized objects are dropped. */
static void rx(const uint8_t *d, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    char c = (char)d[i];
    if (depth == 0 && c != '{')
      continue;
    if (cmd_n < CMD_MAX - 1)
      cmd[cmd_n++] = c;
    else
      overflow = true;
    if (in_str) {
      if (esc)
        esc = false;
      else if (c == '\\')
        esc = true;
      else if (c == '"')
        in_str = false;
      continue;
    }
    if (c == '"')
      in_str = true;
    else if (c == '{')
      depth++;
    else if (c == '}' && --depth == 0) {
      cmd[cmd_n] = '\0';
      if (overflow)
        reply_error("GenericError", "command too large");
      else
        dispatch(cmd);
      cmd_n = 0;
      overflow = false;
    }
  }
}

const struct vserial_client qemuga_client = {QGA_PORT, "org.qemu.guest_agent.0", rx, NULL};

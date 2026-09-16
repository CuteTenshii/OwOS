/* mime.c — filename- and content-based MIME typing (see mime.h). Freestanding:
 * no libc calls, just byte comparisons, so it links into any program. */
#include "mime.h"
#include "owos.h"

/* the extension (after the last '.' in the last path component), or "" */
static const char *ext_of(const char *path)
{
  const char *base = path, *dot = 0;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
      dot = 0;
    } else if (*p == '.')
      dot = p + 1;
  }
  (void)base;
  return dot ? dot : "";
}

static const struct {
  const char *ext, *mime;
} ext_table[] = {
    {"txt", "text/plain"},
    {"text", "text/plain"},
    {"md", "text/markdown"},
    {"csv", "text/csv"},
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"json", "application/json"},
    {"xml", "application/xml"},
    {"c", "text/x-c"},
    {"h", "text/x-c"},
    {"sh", "text/x-shellscript"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"bmp", "image/bmp"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"tar", "application/x-tar"},
    {"mp3", "audio/mpeg"},
    {"wav", "audio/wav"},
    {"ogg", "audio/ogg"},
    {"flac", "audio/flac"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mkv", "video/x-matroska"},
    {"avi", "video/x-msvideo"},
    {"torrent", "application/x-bittorrent"},
    {"sqlite", "application/vnd.sqlite3"},
};

const char *mime_from_ext(const char *path)
{
  const char *e = ext_of(path);
  if (e[0])
    for (unsigned i = 0; i < sizeof ext_table / sizeof ext_table[0]; i++)
      if (streq_ci(e, ext_table[i].ext))
        return ext_table[i].mime;
  return "application/octet-stream";
}

const char *mime_from_magic(const void *data, int len)
{
  const unsigned char *b = (const unsigned char *)data;
  if (len >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
    return "image/png";
  if (len >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    return "image/jpeg";
  if (len >= 4 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8')
    return "image/gif";
  if (len >= 2 && b[0] == 'B' && b[1] == 'M')
    return "image/bmp";
  if (len >= 4 && b[0] == '%' && b[1] == 'P' && b[2] == 'D' && b[3] == 'F')
    return "application/pdf";
  if (len >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 0x03 && b[3] == 0x04)
    return "application/zip";
  if (len >= 2 && b[0] == 0x1F && b[1] == 0x8B)
    return "application/gzip";
  if (len >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F') {
    if (b[8] == 'W' && b[9] == 'A' && b[10] == 'V' && b[11] == 'E')
      return "audio/wav";
    if (b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P')
      return "image/webp";
    if (b[8] == 'A' && b[9] == 'V' && b[10] == 'I' && b[11] == ' ')
      return "video/x-msvideo";
  }
  if (len >= 3 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
    return "audio/mpeg";
  if (len >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3)
    return "video/x-matroska"; /* EBML: WebM/MKV */

  /* No known signature: call it text if every byte is a plausible UTF-8 /
   * printable code unit (allowing common control whitespace), else binary. */
  for (int i = 0; i < len; i++) {
    unsigned char c = b[i];
    if (c == '\t' || c == '\n' || c == '\r')
      continue;
    if (c < 0x20 && c != 0x0C)
      return "application/octet-stream";
    if (c == 0x7F)
      return "application/octet-stream";
  }
  return len ? "text/plain" : "application/octet-stream";
}

int mime_is_text(const char *mime)
{
  const char *t = "text/";
  int pre = 1;
  for (int i = 0; t[i]; i++)
    if (mime[i] != t[i]) {
      pre = 0;
      break;
    }
  if (pre)
    return 1;
  return streq_ci(mime, "application/json") || streq_ci(mime, "application/javascript") ||
         streq_ci(mime, "application/xml") || streq_ci(mime, "image/svg+xml");
}

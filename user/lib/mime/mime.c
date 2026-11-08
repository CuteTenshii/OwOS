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

/* Primary extension first for each type: mime_to_ext returns the first match. */
static const struct {
  const char *ext, *mime;
} ext_table[] = {
    {"txt", "text/plain"},
    {"text", "text/plain"},
    {"log", "text/plain"},
    {"conf", "text/plain"},
    {"ini", "text/plain"},
    {"cfg", "text/plain"},
    {"md", "text/markdown"},
    {"markdown", "text/markdown"},
    {"csv", "text/csv"},
    {"tsv", "text/tab-separated-values"},
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"mjs", "text/javascript"},
    {"cjs", "text/javascript"},
    {"ics", "text/calendar"},
    {"vcf", "text/vcard"},
    {"rtf", "text/rtf"},
    {"c", "text/x-c"},
    {"h", "text/x-c"},
    {"cpp", "text/x-c++"},
    {"cc", "text/x-c++"},
    {"cxx", "text/x-c++"},
    {"hpp", "text/x-c++"},
    {"hh", "text/x-c++"},
    {"hxx", "text/x-c++"},
    {"s", "text/x-asm"},
    {"asm", "text/x-asm"},
    {"java", "text/x-java"},
    {"py", "text/x-python"},
    {"rb", "text/x-ruby"},
    {"pl", "text/x-perl"},
    {"pm", "text/x-perl"},
    {"lua", "text/x-lua"},
    {"go", "text/x-go"},
    {"rs", "text/x-rust"},
    {"php", "text/x-php"},
    {"swift", "text/x-swift"},
    {"diff", "text/x-diff"},
    {"patch", "text/x-diff"},
    {"mk", "text/x-makefile"},
    {"sh", "text/x-shellscript"},
    {"bash", "text/x-shellscript"},
    {"zsh", "text/x-shellscript"},
    {"sql", "text/x-sql"},
    {"tex", "text/x-tex"},
    {"yaml", "text/x-yaml"},
    {"yml", "text/x-yaml"},
    {"toml", "text/x-toml"},
    {"json", "application/json"},
    {"jsonld", "application/ld+json"},
    {"xml", "application/xml"},
    {"xsl", "application/xml"},
    {"xsd", "application/xml"},
    {"xhtml", "application/xhtml+xml"},
    {"xht", "application/xhtml+xml"},
    {"rss", "application/rss+xml"},
    {"atom", "application/atom+xml"},
    {"wasm", "application/wasm"},
    {"jsx", "application/javascript"},
    {"csh", "application/x-sh"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"jpe", "image/jpeg"},
    {"jfif", "image/jpeg"},
    {"gif", "image/gif"},
    {"bmp", "image/bmp"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"svgz", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"cur", "image/x-icon"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"avif", "image/avif"},
    {"heic", "image/heic"},
    {"heif", "image/heic"},
    {"apng", "image/apng"},
    {"psd", "image/vnd.adobe.photoshop"},
    {"ppm", "image/x-portable-pixmap"},
    {"pgm", "image/x-portable-graymap"},
    {"pbm", "image/x-portable-bitmap"},
    {"pnm", "image/x-portable-anymap"},
    {"xbm", "image/x-xbitmap"},
    {"xpm", "image/x-xpixmap"},
    {"mp3", "audio/mpeg"},
    {"mp2", "audio/mpeg"},
    {"mpga", "audio/mpeg"},
    {"wav", "audio/wav"},
    {"wave", "audio/wav"},
    {"ogg", "audio/ogg"},
    {"oga", "audio/ogg"},
    {"spx", "audio/ogg"},
    {"opus", "audio/opus"},
    {"flac", "audio/flac"},
    {"aac", "audio/aac"},
    {"m4a", "audio/mp4"},
    {"aif", "audio/x-aiff"},
    {"aiff", "audio/x-aiff"},
    {"aifc", "audio/x-aiff"},
    {"mid", "audio/midi"},
    {"midi", "audio/midi"},
    {"weba", "audio/webm"},
    {"mka", "audio/x-matroska"},
    {"wma", "audio/x-ms-wma"},
    {"m3u", "audio/x-mpegurl"},
    {"m3u8", "audio/x-mpegurl"},
    {"alac", "audio/x-alac"},
    {"mp4", "video/mp4"},
    {"m4v", "video/mp4"},
    {"webm", "video/webm"},
    {"mkv", "video/x-matroska"},
    {"avi", "video/x-msvideo"},
    {"mpg", "video/mpeg"},
    {"mpeg", "video/mpeg"},
    {"mpe", "video/mpeg"},
    {"ogv", "video/ogg"},
    {"mov", "video/quicktime"},
    {"qt", "video/quicktime"},
    {"flv", "video/x-flv"},
    {"wmv", "video/x-ms-wmv"},
    {"3gp", "video/3gpp"},
    {"ts", "video/mp2t"},
    {"m2ts", "video/mp2t"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttc", "font/collection"},
    {"eot", "application/vnd.ms-fontobject"},
    {"pdf", "application/pdf"},
    {"ps", "application/postscript"},
    {"eps", "application/postscript"},
    {"ai", "application/postscript"},
    {"epub", "application/epub+zip"},
    {"doc", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"xls", "application/vnd.ms-excel"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"odt", "application/vnd.oasis.opendocument.text"},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
    {"odp", "application/vnd.oasis.opendocument.presentation"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"tar", "application/x-tar"},
    {"bz2", "application/x-bzip2"},
    {"xz", "application/x-xz"},
    {"zst", "application/zstd"},
    {"lzma", "application/x-lzma"},
    {"7z", "application/x-7z-compressed"},
    {"rar", "application/vnd.rar"},
    {"z", "application/x-compress"},
    {"cpio", "application/x-cpio"},
    {"iso", "application/x-iso9660-image"},
    {"dmg", "application/x-apple-diskimage"},
    {"deb", "application/vnd.debian.binary-package"},
    {"rpm", "application/x-rpm"},
    {"jar", "application/java-archive"},
    {"apk", "application/vnd.android.package-archive"},
    {"exe", "application/x-msdownload"},
    {"dll", "application/x-msdownload"},
    {"msi", "application/x-msi"},
    {"so", "application/x-sharedlib"},
    {"elf", "application/x-executable"},
    {"o", "application/x-object"},
    {"torrent", "application/x-bittorrent"},
    {"sqlite", "application/vnd.sqlite3"},
    {"sqlite3", "application/vnd.sqlite3"},
    {"db", "application/vnd.sqlite3"},
    {"pem", "application/x-pem-file"},
    {"crt", "application/pkix-cert"},
    {"cer", "application/pkix-cert"},
    {"der", "application/pkix-cert"},
    {"sig", "application/pgp-signature"},
    {"asc", "application/pgp-signature"},
    {"p12", "application/x-pkcs12"},
    {"pfx", "application/x-pkcs12"},
    {"ca", "application/x-x509-ca-cert"},
    {"swf", "application/x-shockwave-flash"},
    {"bin", "application/x-bin"},
    {"kml", "application/vnd.google-earth.kml+xml"},
    {"gpx", "application/gpx+xml"},
    {"lnk", "application/x-ms-shortcut"},
    {"desktop", "application/x-desktop"},
    {"ipynb", "application/x-ipynb+json"},
    {"srt", "application/x-subrip"},
    {"vtt", "text/vtt"},
    {"pub", "application/x-ssh-key"},
};
#define EXT_COUNT ((int)(sizeof ext_table / sizeof ext_table[0]))

const char *mime_from_ext(const char *path)
{
  const char *e = ext_of(path);
  if (e[0])
    for (int i = 0; i < EXT_COUNT; i++)
      if (streq_ci(e, ext_table[i].ext))
        return ext_table[i].mime;
  return "application/octet-stream";
}

/* `a` up to any ";params" and trailing blanks, case-insensitively equal to `b` */
static int mime_eq(const char *a, const char *b)
{
  while (*a && *a != ';' && *a != ' ') {
    char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
    if (x != *b++)
      return 0;
    a++;
  }
  return *b == 0;
}

const char *mime_to_ext(const char *mime)
{
  for (int i = 0; i < EXT_COUNT; i++)
    if (mime_eq(mime, ext_table[i].mime))
      return ext_table[i].ext;
  return 0;
}

int mime_entry(int i, const char **ext, const char **mime)
{
  if (i < 0 || i >= EXT_COUNT)
    return 0;
  *ext = ext_table[i].ext;
  *mime = ext_table[i].mime;
  return 1;
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
  if (streq_ci(mime, "application/json") || streq_ci(mime, "application/javascript") ||
      streq_ci(mime, "application/xml") || streq_ci(mime, "image/svg+xml") || streq_ci(mime, "application/x-sh"))
    return 1;
  int n = 0;
  while (mime[n])
    n++;
  return (n > 4 && streq_ci(mime + n - 4, "+xml")) || (n > 5 && streq_ci(mime + n - 5, "+json"));
}

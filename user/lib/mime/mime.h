/* mime.h — tiny MIME-type helper.
 *
 * Maps filenames and content to MIME types, the way the clipboard
 * (user/lib/clipboard), the Files app and the browser tag what they hold. All
 * returns are static strings — never freed, safe to keep. */
#ifndef OWOS_MIME_H
#define OWOS_MIME_H

/* MIME type for a filename, by its extension (case-insensitive). Returns
 * "application/octet-stream" when the extension is unknown or absent. */
const char *mime_from_ext(const char *path);

/* MIME type sniffed from leading magic bytes (PNG/JPEG/GIF/PDF/ZIP/...). Falls
 * back to "text/plain" when the bytes look like UTF-8 text, else
 * "application/octet-stream". */
const char *mime_from_magic(const void *data, int len);

/* 1 if `mime` is textual: any "text/" subtype, or a known text-bearing
 * application type (json, javascript, xml) or image/svg+xml. */
int mime_is_text(const char *mime);

#endif /* OWOS_MIME_H */

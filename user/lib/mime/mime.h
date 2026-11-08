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

/* Primary file extension (no dot) for a MIME type, or NULL if the type is not in the table. Parameters after
 * ';' are ignored and the match is case-insensitive. */
const char *mime_to_ext(const char *mime);

/* Enumerate the extension table: entry i sets *ext and *mime and returns 1, or returns 0 past the end. The
 * first entry of a type holds its primary extension. */
int mime_entry(int i, const char **ext, const char **mime);

/* MIME type sniffed from leading magic bytes (PNG/JPEG/GIF/PDF/ZIP/...). Falls
 * back to "text/plain" when the bytes look like UTF-8 text, else
 * "application/octet-stream". */
const char *mime_from_magic(const void *data, int len);

/* 1 if `mime` is textual: any "text/" subtype, or a known text-bearing
 * application type (json, javascript, xml, sh), image/svg+xml, or any +xml / +json type. */
int mime_is_text(const char *mime);

#endif /* OWOS_MIME_H */

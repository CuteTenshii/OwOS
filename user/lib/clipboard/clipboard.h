/* clipboard.h — client API for the clipboard server (/bin/clipd).
 *
 * Each call is a short request over the "clipboard" IPC port (connect, one
 * request, close). The clipboard is multi-item and holds arbitrary content: a
 * clip is an opaque blob tagged with a MIME type ("text/plain", "image/png",
 * "application/pdf", "video/mp4", ...), newest first. With no clipd running the
 * calls fail gracefully (put -> -1, get/list -> -1, count -> 0). */
#ifndef OWOS_CLIPBOARD_H
#define OWOS_CLIPBOARD_H

/* Push a clip (any MIME type) to the front of the history. 0, or -1 if clipd is
 * down. */
int clip_put(const char *mime, const void *data, int len);

/* Convenience for the common case: clip_put("text/plain", ...). */
int clip_put_text(const char *text, int len);

/* Fetch clip #idx (0 = newest) into a freshly malloc'd buffer. On success
 * copies the MIME type into mime[mimemax], sets *out (caller frees) and *len,
 * and returns 0; -1 if there is no such item or clipd is unreachable. */
int clip_get(int idx, char *mime, int mimemax, void **out, int *len);

/* Number of items currently in the history (0 if empty or clipd is down). */
int clip_count(void);

/* Describe clip #idx without pulling its whole payload: copies the MIME type
 * into mime[mimemax], sets *full_len, and returns a malloc'd NUL-terminated
 * text *preview (empty for non-text types). Caller frees *preview. 0, or -1 on
 * out-of-range / clipd down. */
int clip_list(int idx, char *mime, int mimemax, int *full_len, char **preview);

#endif /* OWOS_CLIPBOARD_H */

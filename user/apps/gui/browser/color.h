/* color.h — CSS/SVG colour parsing, shared by css.c and svg.c.
 *
 * Parses one colour token into 0xRRGGBB: #hex (3/4/6/8 digits), rgb()/rgba(),
 * hsl()/hsla() (comma or space syntax), oklch(), and the named-colour table.
 * Returns -1 for anything that isn't a colour (so callers can fall through to
 * keywords like `none`/`currentColor`/`inherit`). */
#ifndef OWOS_COLOR_H
#define OWOS_COLOR_H

/* 0xRRGGBB, or -1 if `s` isn't a recognised colour. Leading space is skipped. */
int color_parse(const char *s);

/* Same, and reports the alpha (0..255) via *alpha — 255 when opaque or
 * unspecified, less for rgba()/hsla()/`/alpha`/#RRGGBBAA/transparent. */
int color_parse_a(const char *s, int *alpha);

#endif /* OWOS_COLOR_H */

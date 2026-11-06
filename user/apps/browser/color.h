/* color.h: CSS/SVG colour parsing (hex, rgb, hsl, oklch, named), shared by css.c and svg.c. */
#ifndef OWOS_COLOR_H
#define OWOS_COLOR_H

/* 0xRRGGBB, or -1 if `s` isn't a recognised colour. */
int color_parse(const char *s);

/* Same, also reporting alpha (0..255) via *alpha. */
int color_parse_a(const char *s, int *alpha);

#endif /* OWOS_COLOR_H */
